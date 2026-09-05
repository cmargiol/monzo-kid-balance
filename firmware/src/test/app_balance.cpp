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

#ifndef DISPLAY_TITLE
#define DISPLAY_TITLE "MY MONEY" // personalised in secrets.h, kept out of the repo
#endif
#ifndef DEVICE_TZ
// POSIX timezone rule; default is UK time with summer time. Only the clock
// demo screen shows local time — the balance header's time comes from the
// Worker in UK time regardless.
#define DEVICE_TZ "GMT0BST,M3.5.0/1,M10.5.0"
#endif

#ifdef BALANCE_LIVE
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_sntp.h>
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
        String txNote[3]; // the sender's message (Monzo notes), often empty
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
        {"", "Μπραβο! For being brilliant", ""},
    };
#endif

    static Preferences _prefs;
    static uint32_t _lastFetch = 0;       // file-scope: re-entering the app
                                          // must not force a refetch storm
    static volatile bool _inFlight = false;

    // ------------------------------------------------------------- Power
    //
    // Policy: full brightness whenever recently touched or on USB power; on
    // battery, dim after 20s idle and deep-sleep after 2 min (the front
    // button wakes it; the 200mAh cell can't sustain always-on).
    //
    // USB detection is a heuristic — the PLUS2 has no VBUS-detect pin, only
    // the battery voltage on GPIO38 (ADC, /2 divider). A cell on USB charge
    // sits at/above ~4.1V; discharging under load it sags below ~4.0V.
    // The classifier smooths samples and applies hysteresis, and every
    // misclassification is deliberately RECOVERABLE: worst case the screen
    // dims or deep-sleeps while plugged in (a press wakes it), or stays
    // bright a while just after unplugging. Never a hard power_off() here —
    // on USB that call never returns (rail stays up) and the device hangs.

    static const float USB_ENTER_V = 4.12f; // avg >= this -> treat as USB
    static const float USB_EXIT_V = 4.04f;  // avg <= this -> treat as battery
    static const uint8_t BRIGHT_FULL = 200;
    static const uint8_t BRIGHT_DIM = 40;
    static const uint32_t DIM_AFTER_MS = 20000;
    static const uint32_t SLEEP_AFTER_MS = 120000;

    static uint32_t _lastInput = 0;
    static uint32_t _lastVoltSample = 0;
    static float _voltAvg = 0;
    static bool _usb = true;      // optimistic start: never sleep a fresh boot
    static uint8_t _bright = 0;   // 0 = not yet asserted (LGFX default is 127)

    static float battery_volts()
    {
        return analogReadMilliVolts(38) * 2 / 1000.0f;
    }

    bool TEST::power_on_usb() { return _usb; }
    bool TEST::power_dimmed() { return _bright == BRIGHT_DIM; }
    void TEST::power_input() { _lastInput = millis(); }

    void TEST::power_tick()
    {
        if (millis() - _lastVoltSample > 2000)
        {
            _lastVoltSample = millis();
            float v = battery_volts();
            _voltAvg = _voltAvg == 0 ? v : _voltAvg * 0.75f + v * 0.25f; // ~8s window
            if (_usb && _voltAvg <= USB_EXIT_V)
                _usb = false;
            else if (!_usb && _voltAvg >= USB_ENTER_V)
                _usb = true;
        }

        uint32_t idle = millis() - _lastInput;
        uint8_t want = (!_usb && idle > DIM_AFTER_MS) ? BRIGHT_DIM : BRIGHT_FULL;
        if (want != _bright)
        {
            _bright = want;
            lcd.setBrightness(want);
        }

        if (!_usb && idle > SLEEP_AFTER_MS)
        {
            // Deep sleep, front button (GPIO37, active low) wakes to a fresh
            // boot — the NVS cache puts the balance up instantly. The power-
            // hold pad must stay latched through sleep or the board cuts its
            // own power and only the hardware PWR-on can revive it.
            lcd.setBrightness(0);
            gpio_hold_en((gpio_num_t)POWER_HOLD_PIN);
            gpio_deep_sleep_hold_en();
            esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);
            esp_deep_sleep_start();
        }
    }

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

    // Secondary-text gray, used for the clock and captions.
    #define COL_DIM (Disbuff->color565(150, 150, 150))
    // Message lines: brighter for readability, still distinct from the white
    // transaction names.
    #define COL_NOTE (Disbuff->color565(205, 205, 205))

    /** Truncate to `max` chars, last one becoming "." — for strings that
     *  must never collide with or overflow their line. */
    static String clip(const String &s, int max)
    {
        if ((int)s.length() <= max)
            return s;
        return s.substring(0, max - 1) + ".";
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
            if (c >= 0xCD && c <= 0xCF && i + 1 < in.length())
            {
                // Greek (and Coptic) pass through: the message line renders
                // in efontCN_16, which carries the Greek alphabet.
                out += (char)c;
                out += in[++i];
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
        Disbuff->setTextColor(COL_DIM);
        Disbuff->drawRightString(sanitise(_data.updated).c_str(), 234, 5, 1);
        if (_inFlight)
            Disbuff->fillCircle(160, 12, 4, TFT_SKYBLUE); // clear of the HH:MM text

        if (txScreen)
        {
            // Clamp defensively: txCount comes from a parsed network payload.
            int shown = _data.txCount < 3 ? _data.txCount : 3;
            for (int i = 0; i < shown; i++)
            {
                // Two lines per row: sender + amount, then the sender's
                // message (if any) smaller and gray underneath.
                int y = 30 + i * 34;
                String amt = sanitise(_data.txAmt[i]);
                String name = sanitise(_data.txName[i]);
                // Clip the name to the space left of the right-aligned amount
                // (Font0 size 2 advances 12px/char) so they can never collide.
                int maxName = (234 - (int)amt.length() * 12 - 6 - 12) / 12;
                if (maxName < 1) maxName = 1;
                name = clip(name, maxName);
                Disbuff->setFont(&fonts::Font0);
                Disbuff->setTextSize(2);
                Disbuff->setTextColor(TFT_WHITE);
                Disbuff->setCursor(6, y);
                Disbuff->printf("%s", name.c_str());
                Disbuff->setTextColor(amt[0] == '+' ? TFT_GREEN : TFT_WHITE);
                Disbuff->drawRightString(amt.c_str(), 234, y, 1);

                String note = sanitise(_data.txNote[i]);
                if (note.length() > 0)
                {
                    // The Worker caps notes to fit this line (width-aware:
                    // Greek glyphs are double-width); this clip is defense
                    // against a non-Worker payload only.
                    note = clip(note, 60);
                    // efontCN_16: the only bundled font with Greek glyphs.
                    Disbuff->setFont(&fonts::efontCN_16);
                    Disbuff->setTextSize(1);
                    Disbuff->setTextColor(COL_NOTE);
                    Disbuff->setCursor(6, y + 17);
                    Disbuff->printf("%s", note.c_str());
                }
            }
            if (shown == 0)
            {
                Disbuff->setFont(&fonts::Font0);
                Disbuff->setTextSize(2);
                Disbuff->setTextColor(COL_DIM);
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
            Disbuff->setTextColor(COL_DIM);
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
            _prefs.putString((String("txm") + i).c_str(), _data.txNote[i]);
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
            _data.txNote[i] = _prefs.getString((String("txm") + i).c_str(), "");
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
        // Start the NTP client once per boot, whatever the clock says — a
        // deep-sleep wake or a restart carries over a plausible-looking time
        // that drifted on the sleep oscillator (minutes per day). It then
        // re-syncs every few hours for the rest of the uptime (the awake
        // clock runs off the crystal and drifts about a second a day), and
        // each completed sync is what updates the clock chip (see
        // balance_app). This also (re)applies the DEVICE_TZ timezone, which
        // a restart forgets.
        static bool sntpStarted = false;
        if (!sntpStarted)
        {
            configTzTime(DEVICE_TZ, "pool.ntp.org", "time.google.com");
            sntpStarted = true;
        }
        if (time(nullptr) > 1700000000) // plausible: don't hold up the fetch
            return true;
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
            out.txNote[out.txCount] = sanitise((const char *)(t[2] | ""));
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
        bool wasInFlight = _inFlight;
#endif
        bool prevDimmed = false;

        while (1)
        {
            // Policy tick happens via checkReboot() below (shared with every
            // demo screen); here we only need the dimmed flag for input
            // semantics — a press on a dimmed screen wakes, it doesn't act.
            bool dimmed = power_dimmed();
#ifdef BALANCE_LIVE
            if (prevDimmed && !dimmed && millis() - _lastFetch > 60000)
            {
                _lastFetch = 0; // waking to stale data forces a refresh
            }
#endif
            prevDimmed = dimmed;

#ifdef BALANCE_LIVE
            const uint32_t FETCH_EVERY_MS = power_on_usb() ? 60000 : 300000;
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
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)
            {
                // Reported once per completed sync. Only a fresh sync is
                // worth writing into the clock chip: after a deep-sleep wake
                // the carried-over time has drifted, and the device may
                // sleep again before any later correction.
                rtc_set_from_localtime();
            }
#endif
            if (dirty)
            {
                balance_draw(txScreen);
                dirty = false;
            }

            if (btnA.pressed())
            {
                power_input();
                if (dimmed)
                {
                    // Wake-only press: power_tick() brightens on the next
                    // pass; no action, and no blocking on the release —
                    // the press edge won't re-fire until a new press.
                }
                else
                {
                    // Short press (acts on release) toggles screens; a long
                    // press fires AT the 600ms threshold, while still held —
                    // waiting for release made every action feel a second late.
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
                }
                power_input();
                dirty = true;
            }

            if (btnB.pressed())
            {
                power_input();
                if (!dimmed)
                {
                    _tone(5500, 50);
                    return; // next app in the demo cycle
                }
            }

            checkReboot();
            delay(10);
        }
    }

}
