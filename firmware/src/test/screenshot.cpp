/**
 * Framebuffer dump over USB serial, for documentation screenshots — mock
 * builds only, so a real balance can never end up in a PNG by accident.
 *
 * Send the byte 'S' (scripts/screenshot.py does this) and the device replies
 * with a header line, the sprite's raw pixel bytes, and a trailer line.
 * checkReboot() polls for the trigger, and every screen's loop calls that,
 * so any screen can be captured. The raw bytes are the sprite's own format
 * (8-bit RGB332 as configured in test_lcd.cpp); the header states bytes per
 * pixel and the row stride so the script needs no assumptions.
 */
#include "test.h"

#ifndef BALANCE_LIVE

namespace TEST
{

    void TEST::screenshot()
    {
        int w = Disbuff->width();
        int h = Disbuff->height();
        uint32_t len = Disbuff->bufferLength();
        int stride = (int)(len / h);        // bytes per row, padding included
        int bpp = stride / w;               // bytes per pixel
        Serial.printf("\nSCREENSHOT %d %d %d %d\n", w, h, bpp, stride);
        Serial.write((const uint8_t *)Disbuff->getBuffer(), len);
        Serial.print("\nSCREENSHOT_END\n");
    }

}

#endif
