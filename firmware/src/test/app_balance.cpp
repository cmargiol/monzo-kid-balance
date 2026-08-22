/**
 * Balance app — the default screen: shows the pocket-money balance served by
 * the Worker, with a second sub-screen for the last three transactions.
 *
 * This version renders MOCK data only (the payload contract mirrors the
 * Worker's /display JSON); the HTTPS fetch replaces the mock in the next PR.
 *
 * Controls, matching the demo idiom:
 *   A short press  — toggle balance <-> transactions
 *   A long press   — (mock only) cycle display states ok/error/needs_reauth,
 *                    so every screen can be previewed on-device
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

namespace TEST
{

    enum BalanceState
    {
        BAL_OK,
        BAL_STALE,       // worker unreachable / data stale: show last-good + strip
        BAL_NEEDS_REAUTH // Monzo SCA lapsed: needs a grown-up
    };

    // Mirrors the Worker /display payload: pre-formatted strings only.
    struct DisplayData
    {
        BalanceState state;
        const char *balance;
        const char *today;
        const char *updated;
        int txCount;
        const char *txName[3];
        const char *txAmt[3];
    };

    static DisplayData _mock = {
        BAL_OK,
        "£12.34",
        "£1.20 spent today",
        "12:00",
        3,
        {"Toy Shop", "Pocket money", "Bus"},
        {"-£4.99", "+£2.00", "-£1.75"},
    };

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

    /** Copy `in` to `out` minus any UTF-8 "£" (0xC2 0xA3) — fonts lack it. */
    static void stripPound(const char *in, char *out, size_t n)
    {
        size_t o = 0;
        for (size_t i = 0; in[i] != '\0' && o < n - 1; i++)
        {
            if ((uint8_t)in[i] == 0xC2 && (uint8_t)in[i + 1] == 0xA3)
            {
                i++; // skip both bytes
                continue;
            }
            out[o++] = in[i];
        }
        out[o] = '\0';
    }

    void TEST::balance_draw(bool txScreen)
    {
        Disbuff->fillRect(0, 0, 240, 135, TFT_BLACK);

        if (_mock.state == BAL_NEEDS_REAUTH)
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

        // Header bar: title + last update time.
        Disbuff->fillRect(0, 0, 240, 25, Disbuff->color565(20, 20, 20));
        Disbuff->setFont(&fonts::Font0);
        Disbuff->setTextSize(2);
        Disbuff->setTextColor(TFT_WHITE);
        Disbuff->setCursor(6, 5);
        // "%s" deliberately: DISPLAY_TITLE is user-personalised and must never
        // be interpreted as a format string.
        Disbuff->printf("%s", txScreen ? "LAST 3" : DISPLAY_TITLE);
        Disbuff->setTextColor(Disbuff->color565(150, 150, 150));
        Disbuff->drawRightString(_mock.updated, 234, 5, 1);

        if (txScreen)
        {
            // Clamp defensively: txCount will come from a parsed network
            // payload once the fetch lands, and the arrays hold exactly 3.
            int shown = _mock.txCount < 3 ? _mock.txCount : 3;
            for (int i = 0; i < shown; i++)
            {
                int y = 36 + i * 26;
                Disbuff->setFont(&fonts::Font0);
                Disbuff->setTextSize(2);
                Disbuff->setTextColor(TFT_WHITE);
                Disbuff->setCursor(6, y);
                Disbuff->printf("%s", _mock.txName[i]);
                bool credit = _mock.txAmt[i][0] == '+';
                char amt[12];
                stripPound(_mock.txAmt[i], amt, sizeof(amt)); // no £ glyph in Font0
                Disbuff->setTextColor(credit ? TFT_GREEN : TFT_WHITE);
                Disbuff->drawRightString(amt, 234, y, 1);
            }
            if (_mock.txCount == 0)
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
            char digits[16];
            bool negative = _mock.balance[0] == '-';
            stripPound(negative ? _mock.balance + 1 : _mock.balance, digits, sizeof(digits));

            Disbuff->setFont(&fonts::DejaVu56);
            Disbuff->setTextSize(1);
            int poundH = 40;
            int poundW = (poundH * 3) / 5 + poundH / 10;
            if (poundW + Disbuff->textWidth(digits) > 232)
            {
                Disbuff->setFont(&fonts::DejaVu40); // fits £100+ balances
            }
            int total = (negative ? 18 : 0) + poundW + Disbuff->textWidth(digits);
            int x = (240 - total) / 2;
            int y = 44;
            Disbuff->setTextColor(TFT_WHITE);
            if (negative)
            {
                Disbuff->fillRoundRect(x, y + poundH / 2 - 3, 12, 6, 2, TFT_WHITE);
                x += 18;
            }
            x += drawPound(Disbuff, x, y + 8, poundH, TFT_WHITE);
            Disbuff->drawString(digits, x, y);

            char today[32];
            stripPound(_mock.today, today, sizeof(today));
            Disbuff->setFont(&fonts::Font0);
            Disbuff->setTextSize(2);
            Disbuff->setTextColor(Disbuff->color565(150, 150, 150));
            Disbuff->drawCenterString(today, 120, 108);
        }

        if (_mock.state == BAL_STALE)
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

    void TEST::balance_app()
    {
        bool txScreen = false;
        bool dirty = true; // redraw only on change: a 100Hz full-sprite push
                           // would saturate the SPI bus for a static screen

        while (1)
        {
            if (dirty)
            {
                balance_draw(txScreen);
                dirty = false;
            }

            if (btnA.pressed())
            {
                // Distinguish short press (toggle screens) from long press
                // (mock-only: cycle states so each variant is previewable).
                uint32_t t0 = millis();
                while (!btnA.read()) // still held
                {
                    delay(10);
                }
                if (millis() - t0 > 800)
                {
                    _mock.state = (BalanceState)((_mock.state + 1) % 3);
                    _tone(4000, 80);
                }
                else
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
