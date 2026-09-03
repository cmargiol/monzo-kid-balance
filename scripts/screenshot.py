#!/usr/bin/env python3
"""Capture the gadget's screen over USB as a PNG.

    .venv/bin/python scripts/screenshot.py docs/images/balance.png docs/images/last3.png

For each file: navigate the device to the screen you want, press Enter, and
the frame is saved as a pixel-exact PNG scaled 3x. Needs a mock build on the
device (the live build has no screenshot hook, so real balances can't leak
into the docs). pyserial comes with the PlatformIO venv.
"""
import glob
import struct
import sys
import time
import zlib

import serial

SCALE = 3


def find_port():
    ports = glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/ttyUSB*")
    if not ports:
        sys.exit("no USB serial device found — is the M5Stick plugged in?")
    return ports[0]


def open_port(path):
    # Keep DTR and RTS low before opening: their edges drive the board's
    # auto-reset circuit, and a reset would lose the screen you navigated to.
    port = serial.Serial()
    port.port = path
    port.baudrate = 115200
    port.timeout = 3
    port.dtr = False
    port.rts = False
    port.open()
    time.sleep(2.5)  # some USB chips reset the board on open; let it boot
    return port


def capture(port):
    """Return (w, h, rgb888 bytes), or None if the reply was garbled."""
    port.reset_input_buffer()
    header = None
    for _ in range(5):
        port.write(b"S")  # re-sent a few times in case the board was still booting
        port.timeout = 3
        line = port.readline()
        while line and b"SCREENSHOT " not in line:
            line = port.readline()
        if line:
            header = line
            break
    if header is None:
        sys.exit("no reply — is the device running a mock build?")
    try:
        w, h, bpp, stride = (int(v) for v in header.split(b"SCREENSHOT ", 1)[1].split()[:4])
    except ValueError:
        return None
    if bpp != 1:
        sys.exit(f"unsupported {bpp} bytes/pixel — the sprite is 8-bit in test_lcd.cpp")
    port.timeout = 15  # the frame takes ~3s at 115200 baud
    raw = port.read(stride * h)
    trailer = port.read(len(b"\nSCREENSHOT_END\n"))
    if len(raw) != stride * h or b"SCREENSHOT_END" not in trailer:
        return None
    return w, h, rgb332_to_rgb888(raw, w, h, stride)


def rgb332_to_rgb888(raw, w, h, stride):
    # The same expansion the graphics library uses when it pushes the sprite
    # to the panel, so the PNG matches the screen exactly.
    table = []
    for v in range(256):
        r, g, b = v >> 5, (v >> 2) & 7, v & 3
        table.append(bytes((r * 36 + (r >> 1), g * 36 + (g >> 1), b * 0x55)))
    out = bytearray()
    for y in range(h):
        row = raw[y * stride:y * stride + w]
        out += b"".join(table[v] for v in row)
    return bytes(out)


def png(w, h, rgb):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    rows = []
    for y in range(h):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        scaled = b"".join(row[x * 3:x * 3 + 3] * SCALE for x in range(w))
        rows.extend([b"\x00" + scaled] * SCALE)
    ihdr = struct.pack(">IIBBBBB", w * SCALE, h * SCALE, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))


def main(paths):
    if not paths or paths[0] in ("-h", "--help"):
        print(__doc__)
        return
    port = open_port(find_port())
    for path in paths:
        input(f"Navigate the device to the screen for {path}, then press Enter… ")
        while True:
            result = capture(port)
            if result:
                break
            input("Garbled capture — press Enter to retry… ")
        w, h, rgb = result
        with open(path, "wb") as f:
            f.write(png(w, h, rgb))
        print(f"saved {path} ({w * SCALE}x{h * SCALE})")


if __name__ == "__main__":
    main(sys.argv[1:])
