/**
 * Balance app — the default screen: shows the pocket-money balance served by
 * the Worker, with a second sub-screen for the last three transactions.
 *
 * Two build modes:
 *  - secrets.h present with WORKER_URL: live mode — HTTPS GET the Worker's
 *    /display over verified TLS (root bundle in ca_certs.h), cache the last
 *    good payload in NVS so a cold boot shows a balance instantly. The fetch
 *    runs on its own FreeRTOS task so the UI (buttons, PWR reboot) never
 *    blocks on WiFi/NTP/TLS — timeouts add up to ~35s worst-case.
 *  - no secrets.h: mock mode — renders fixed data; a long press on A cycles
 *    the three display states for on-device preview. Keeps the public repo
 *    building green with zero configuration.
 *
 * Controls, matching the demo idiom:
 *   A short press  — toggle balance <-> transactions
 *   A long press   — live: refresh now · mock: cycle display states
 *   B              — leave to the next app in the demo cycle
 *   PWR            — handled by checkReboot(), unchanged
 */
#include "test.h"

#if __has_include("../secrets.h")
#include "../secrets.h"
#endif
#ifndef DISPLAY_TITLE
#define DISPLAY_TITLE "MY MONEY" // personalised in secrets.h, kept out of the repo
#endif

#ifdef WORKER_URL
#define BALANCE_LIVE 1
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "../ca_certs.h"
#endif

namespace TEST
{

    enum BalanceState
    {
        BAL_OK,
        BAL_STALE,       // fetch failing / data stale: show last-good + strip
        BAL_NEEDS_REAUTH // Monzo SCA lapsed: needs a grown-up
    };

    // Mirrors the Worker /display payload: pre-formatted strings only.
    struct DisplayData
    {
        BalanceState state;
        String balance;
        String today;
        String updated;
        int txCount;
        String txName[3];
        String txAmt[3];
    };

#ifdef BALANCE_LIVE
    static DisplayData _data = {BAL_STALE, "--", "Not fetched yet", "--:--", 0, {}, {}};
#else
    static DisplayData _data = {
        BAL_OK,
        "£12.34",
        "£1.20 spent today",
        "12:00",
        3,
        {"Toy Shop", "Pocket money", "Bus"},
        {"-£4.99", "+£2.00", "-£1.75"},
    };
#endif

    static Preferences _prefs;
    static uint32_t _lastFetch = 0;       // file-scope: re-entering the app
                                          // must not force a refetch storm
    static volatile bool _inFlight = false;

    /**
     * No font bundled with M5GFX 0.1.11 contains "£" (DejaVu* and Font0 are
     * ASCII 0x20-0x7E; the efont CJK sets skip Latin-1), so the symbol is
     * drawn geometrically: hook, stem, crossbar, base. h = glyph height.
     * Returns the glyph's advance width.
     */
    static int drawPound(LGFX_Sprite *s, int x, int y, int h, uint16_t color)
    {
        int t = h / 7;              // stroke thickness
        int w = (h * 3) / 5;        // glyph width
        int stemX = x + w / 4;
        s->fillRoundRect(stemX, y + t / 2, t, h - t, t / 3, color);          // stem
        s->fillRoundRect(stemX, y, w - w / 4, t, t / 3, color);              // top hook
        s->fillRoundRect(x, y + (h - t) / 2, (w * 3) / 4, t, t / 3, color);  // crossbar
        s->fillRoundRect(x, y + h - t, w, t, t / 3, color);                  // base
        return w + h / 10;
    }

    /**
     * The fonts are ASCII-only, so every payload string passes through this:
     * "£" (C2 A3) is dropped (drawn geometrically where wanted), "—" (E2 80
     * 94) becomes "-", "…" (E2 80 A6) becomes "...", anything else non-ASCII
     * is skipped whole (no stray continuation bytes rendered as junk).
     */
    static String sanitise(const String &in)
    {
        String out;
        out.reserve(in.length());
        for (unsigned i = 0; i < in.length(); i++)
        {
            uint8_t c = in[i];
            if (c < 0x80)
            {
                out += (char)c;
                continue;
            }
            if (c == 0xC2 && i + 1 < in.length() && (uint8_t)in[i + 1] == 0xA3)
            {
                i++;
                continue;
            }
            if (c == 0xE2 && i + 2 < in.length() && (uint8_t)in[i + 1] == 0x80)
            {
                uint8_t b3 = in[i + 2];
                if (b3 == 0x94) out += '-';
                if (b3 == 0xA6) out += "...";
                i += 2;
                continue;
            }
            while (i + 1 < in.length() && ((uint8_t)in[i + 1] & 0xC0) == 0x80)
                i++; // skip the rest of an unknown multi-byte sequence
        }
        return out;
    }

    void TEST::balance_draw(bool txScreen)
    {
        Disbuff->fillRect(0, 0, 240, 135, TFT_BLACK);

        if (_data.state == BAL_NEEDS_REAUTH)
        {
            // Full-screen, kid-readable: this needs a parent, nothing else works.
            Disbuff->pushImage(96, 12, 48, 48, (uint16_t *)error_48);
            Disbuff->setFont(&fonts::Font0);
            Disbuff->setTextSize(3);
            Disbuff->setTextColor(TFT_ORANGE);
            Disbuff->drawCenterString("Ask Dad", 120, 68);
            Disbuff->drawCenterString("to fix me!", 120, 96);
            Displaybuff();
            return;
        }

        // Header bar: title + last update time (+ fetch-in-progress dot).
        Disbuff->fillRect(0, 0, 240, 25, Disbuff->color565(20, 20, 20));
        Disbuff->setFont(&fonts::Font0);
        Disbuff->setTextSize(2);
        Disbuff->setTextColor(TFT_WHITE);
        Disbuff->setCursor(6, 5);
        // "%s" deliberately: DISPLAY_TITLE is user-personalised and must never
        // be interpreted as a format string.
        Disbuff->printf("%s", txScreen ? "LAST 3" : DISPLAY_TITLE);
        Disbuff->setTextColor(Disbuff->color565(150, 150, 150));
        Disbuff->drawRightString(sanitise(_data.updated).c_str(), 234, 5, 1);
        if (_inFlight)
            Disbuff->fillCircle(160, 12, 4, TFT_SKYBLUE); // clear of the HH:MM text

        if (txScreen)
        {
            // Clamp defensively: txCount comes from a parsed network payload.
            int shown = _data.txCount < 3 ? _data.txCount : 3;
            for (int i = 0; i < shown; i++)
            {
                int y = 36 + i * 26;
                String amt = sanitise(_data.txAmt[i]);
                String name = sanitise(_data.txName[i]);
                // Clip the name to the space left of the right-aligned amount
                // (Font0 size 2 advances 12px/char) so they can never collide.
                int maxName = (234 - (int)amt.length() * 12 - 6 - 12) / 12;
                if (maxName < 1) maxName = 1;
                if ((int)name.length() > maxName)
                    name = name.substring(0, maxName - 1) + ".";
                Disbuff->setFont(&fonts::Font0);
                Disbuff->setTextSize(2);
                Disbuff->setTextColor(TFT_WHITE);
                Disbuff->setCursor(6, y);
                Disbuff->printf("%s", name.c_str());
                Disbuff->setTextColor(amt[0] == '+' ? TFT_GREEN : TFT_WHITE);
                Disbuff->drawRightString(amt.c_str(), 234, y, 1);
            }
            if (shown == 0)
            {
                Disbuff->setFont(&fonts::Font0);
                Disbuff->setTextSize(2);
                Disbuff->setTextColor(Disbuff->color565(150, 150, 150));
                Disbuff->drawCenterString("Nothing yet!", 120, 64);
            }
        }
        else
        {
            // Big balance: hand-drawn £ + DejaVu digits, centered as one unit.
            bool negative = _data.balance[0] == '-' && _data.balance.length() > 2;
            String digits = sanitise(negative ? _data.balance.substring(1) : _data.balance);

            if (digits.length() == 0 || digits == "-")
            {
                // Placeholder payload (fresh deploy, nothing fetched yet).
                Disbuff->setFont(&fonts::DejaVu56);
                Disbuff->setTextColor(Disbuff->color565(120, 120, 120));
                Disbuff->drawCenterString("--", 120, 44);
            }
            else
            {
                Disbuff->setFont(&fonts::DejaVu56);
                Disbuff->setTextSize(1);
                int poundH = 40;
                int poundW = (poundH * 3) / 5 + poundH / 10;
                if (poundW + Disbuff->textWidth(digits.c_str()) > 232)
                {
                    Disbuff->setFont(&fonts::DejaVu40); // fits £100+ balances
                }
                int total = (negative ? 18 : 0) + poundW + Disbuff->textWidth(digits.c_str());
                int x = (240 - total) / 2;
                int y = 44;
                Disbuff->setTextColor(TFT_WHITE);
                if (negative)
                {
                    Disbuff->fillRoundRect(x, y + poundH / 2 - 3, 12, 6, 2, TFT_WHITE);
                    x += 18;
                }
                x += drawPound(Disbuff, x, y + 8, poundH, TFT_WHITE);
                Disbuff->drawString(digits.c_str(), x, y);
            }

            Disbuff->setFont(&fonts::Font0);
            Disbuff->setTextSize(2);
            Disbuff->setTextColor(Disbuff->color565(150, 150, 150));
            // y=96: leaves the stale strip's band (115..135) untouched.
            Disbuff->drawCenterString(sanitise(_data.today).c_str(), 120, 96);
        }

        if (_data.state == BAL_STALE)
        {
            // Last-good data is still shown; the strip explains it's old.
            Disbuff->fillRect(0, 115, 240, 20, Disbuff->color565(120, 60, 0));
            Disbuff->setFont(&fonts::Font0);
            Disbuff->setTextSize(2);
            Disbuff->setTextColor(TFT_ORANGE);
            Disbuff->drawCenterString("Tell Dad to check me!", 120, 118);
        }

        Displaybuff();
    }

#ifdef BALANCE_LIVE

    // The fetch task writes ONLY into _staging and then raises _done; the UI
    // loop is the only writer of _data and only reads _staging after _done.
    static DisplayData _staging;
    static volatile bool _done = false;

    /** Persist only healthy payloads: a degraded one must never poison the
     *  cache that cold boots replay as-good. */
    static void cacheSave()
    {
        _prefs.putString("bal", _data.balance);
        _prefs.putString("today", _data.today);
        _prefs.putString("upd", _data.updated);
        _prefs.putInt("txn", _data.txCount);
        for (int i = 0; i < 3; i++)
        {
            _prefs.putString((String("txn") + i).c_str(), _data.txName[i]);
            _prefs.putString((String("txa") + i).c_str(), _data.txAmt[i]);
        }
    }

    static void cacheLoad()
    {
        if (!_prefs.isKey("bal"))
            return;
        _data.balance = _prefs.getString("bal", "--");
        _data.today = _prefs.getString("today", "");
        _data.updated = _prefs.getString("upd", "--:--");
        _data.txCount = _prefs.getInt("txn", 0);
        for (int i = 0; i < 3; i++)
        {
            _data.txName[i] = _prefs.getString((String("txn") + i).c_str(), "");
            _data.txAmt[i] = _prefs.getString((String("txa") + i).c_str(), "");
        }
        // Only ok-state payloads are ever saved, so cached data renders clean
        // while the first fetch is in flight; the strip appears on failure.
        _data.state = BAL_OK;
    }

    static bool ensure_wifi()
    {
        if (WiFi.status() == WL_CONNECTED)
            return true;
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
        {
            delay(100);
        }
        return WiFi.status() == WL_CONNECTED;
    }

    /** TLS cert validation needs sane wall-clock time (within years). */
    static bool ensure_time()
    {
        if (time(nullptr) > 1700000000) // any post-2023 time = already synced
            return true;
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        uint32_t t0 = millis();
        while (time(nullptr) < 1700000000 && millis() - t0 < 10000)
        {
            delay(100);
        }
        return time(nullptr) > 1700000000;
    }

    /** Runs on the fetch task. Fills `out`; out.state stays BAL_STALE on any
     *  failure path so a forgotten assignment can't render stale data as ok. */
    static void fetch_into(DisplayData &out)
    {
        out.state = BAL_STALE;

        if (!ensure_wifi() || !ensure_time())
            return;

        uint32_t heapBefore = ESP.getFreeHeap();
        WiFiClientSecure client;
        client.setCACert(CA_CERT_BUNDLE);
        HTTPClient http;
        http.setTimeout(10000);
        if (!http.begin(client, WORKER_URL))
            return;
        http.addHeader("Authorization", "Bearer " DEVICE_TOKEN);
        int code = http.GET();
        if (code != 200)
        {
            char tlsErr[128] = {0};
            client.lastError(tlsErr, sizeof(tlsErr));
            printf("balance fetch: HTTP %d (%s; tls: %s; heap %u->%u)\n",
                   code, http.errorToString(code).c_str(), tlsErr,
                   heapBefore, ESP.getFreeHeap());
            http.end();
            return;
        }

        StaticJsonDocument<2048> doc;
        DeserializationError err = deserializeJson(doc, http.getString());
        http.end();
        if (err)
        {
            printf("balance fetch: json %s\n", err.c_str());
            return;
        }

        const char *st = doc["state"] | "error";
        out.balance = sanitise((const char *)(doc["balance"] | "--"));
        out.today = sanitise((const char *)(doc["today"] | ""));
        out.updated = sanitise((const char *)(doc["updated"] | "--:--"));
        out.txCount = 0;
        for (JsonArray t : doc["tx"].as<JsonArray>())
        {
            if (out.txCount >= 3)
                break;
            out.txName[out.txCount] = sanitise((const char *)(t[0] | "?"));
            out.txAmt[out.txCount] = sanitise((const char *)(t[1] | ""));
            out.txCount++;
        }
        out.state = strcmp(st, "needs_reauth") == 0 ? BAL_NEEDS_REAUTH
                    : strcmp(st, "ok") == 0         ? BAL_OK
                                                    : BAL_STALE;
        printf("balance fetch: state %s, heap %u\n", st, ESP.getFreeHeap());
    }

    static void fetch_task(void *)
    {
        fetch_into(_staging);
        _done = true;
        _inFlight = false;
        vTaskDelete(nullptr);
    }

    /** Kick off an async fetch (no-op if one is already running). */
    bool TEST::balance_fetch()
    {
        if (_inFlight)
            return false;
        _inFlight = true;
        _done = false;
        // 12KB stack: TLS handshake peaks ~8KB of stack (buffers are heap).
        xTaskCreatePinnedToCore(fetch_task, "balfetch", 12288, nullptr, 1, nullptr, 0);
        return true;
    }

#endif // BALANCE_LIVE

    void TEST::balance_app()
    {
        bool txScreen = false;
        bool dirty = true; // redraw only on change: a 100Hz full-sprite push
                           // would saturate the SPI bus for a static screen

#ifdef BALANCE_LIVE
        // A copied-but-unfilled secrets.h must not fetch-loop against a
        // placeholder network; say what's wrong on screen instead.
        static const bool configured = strcmp(WIFI_SSID, "FILL_ME_IN") != 0;
        static bool booted = false;
        if (!booted)
        {
            _prefs.begin("balance");
            cacheLoad(); // instant last-known balance on cold boot
            if (!configured)
            {
                _data.state = BAL_STALE;
                _data.today = "Fill in secrets.h!";
            }
            booted = true;
        }
        const uint32_t FETCH_EVERY_MS = 60000;
        bool wasInFlight = _inFlight;
#endif

        while (1)
        {
#ifdef BALANCE_LIVE
            // Completion MUST be handled before deciding whether a fetch is
            // due: balance_fetch() clears _done, so in the old order a fresh
            // fetch started first and swallowed every completion — _lastFetch
            // never advanced and the device fetch-looped forever.
            if (_done)
            {
                _done = false;
                _lastFetch = millis();
                if (_staging.state == BAL_OK)
                {
                    _data = _staging;
                    cacheSave();
                }
                else if (_staging.balance.length() <= 2)
                {
                    // Placeholder payload (e.g. Worker's fresh-deploy fallback)
                    // or failed fetch: keep last-good strings, adopt the state.
                    _data.state = _staging.state;
                }
                else
                {
                    _data = _staging; // Worker sent last-good strings + state
                }
                dirty = true;
            }
            if (configured && !_inFlight && (_lastFetch == 0 || millis() - _lastFetch > FETCH_EVERY_MS))
            {
                // Heap headroom for the ~50KB TLS handshake comes from the
                // sprite being 8-bit (see test_lcd.cpp) — no sprite juggling.
                balance_fetch();
                dirty = true; // in-flight dot appears
            }
            if (wasInFlight != _inFlight)
            {
                wasInFlight = _inFlight;
                dirty = true; // header dot appears/disappears
            }
#endif
            if (dirty)
            {
                balance_draw(txScreen);
                dirty = false;
            }

            if (btnA.pressed())
            {
                // Short press (acts on release) toggles screens; a long press
                // fires AT the 600ms threshold, while still held — waiting
                // for release made every action feel a second late.
                uint32_t t0 = millis();
                bool longFired = false;
                while (!btnA.read()) // still held
                {
                    if (!longFired && millis() - t0 > 600)
                    {
                        longFired = true;
                        _tone(4000, 80); // feedback under the finger
#ifdef BALANCE_LIVE
                        _lastFetch = 0; // force refresh on next loop pass
#else
                        _data.state = (BalanceState)((_data.state + 1) % 3);
                        balance_draw(txScreen); // show the new state immediately
#endif
                    }
                    delay(10);
                }
                if (!longFired)
                {
                    txScreen = !txScreen;
                    _tone(5000, 50);
                }
                dirty = true;
            }

            if (btnB.pressed())
            {
                _tone(5500, 50);
                return; // next app in the demo cycle
            }

            checkReboot();
            delay(10);
        }
    }

}
