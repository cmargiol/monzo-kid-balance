# Firmware — M5StickC PLUS2

The gadget's firmware: M5Stack's factory demo (all its toy screens intact)
with a **Balance app** added as the default screen. The device never talks to
Monzo — it fetches a pre-formatted payload from the Worker (`../worker/`) over
verified TLS and draws it.

Provenance of the vendored demo and how to restore stock firmware: `UPSTREAM.md`.

## What's on the screen

Boot lands on the Balance screen. Controls:

| Button | Short press | Long press (~0.6s) |
|---|---|---|
| **A** (big front button) | Balance ↔ LAST 3 transactions | Live: refresh now · Mock: cycle display states |
| **B** (right side) | Leave to the factory demo cycle (gyro cube, clock, mic, IR, WiFi scan, BLE); the cycle wraps back to Balance | — |
| **PWR** (left side) | Hold ~1s and release: software restart | Keep holding ~3s: hardware power-off (press ~2s to turn on) |

A press on a dimmed screen only wakes it.

Screens:

- **Balance** — big amount (£ drawn geometrically: no bundled font has the
  glyph), "spent today" caption, last-update time (UK time, from the Worker).
- **LAST 3** — sender, amount (green = money in), and the sender's message in
  a smaller line underneath (Greek renders; accents are stripped server-side;
  emoji can't be shown).
- **Stale** — last-good data with an orange "Tell Dad to check me!" strip
  (Worker unreachable, or the Worker itself reports stale/error data).
- **Needs re-approval** — full-screen "Ask Dad to fix me!" (Monzo's ~90-day
  SCA lapse; the Worker also emails the parent).

A blue dot in the header means a fetch is in flight.

## Two build modes

- **Mock** — no `src/secrets.h`: fixed demo data, long-press A cycles the
  three display states. Builds with zero configuration (what a fresh clone
  gets). Flash ≈ 80%.
- **Live** — `src/secrets.h` defines `WORKER_URL`: real fetching. Flash ≈ 85%.

`src/secrets.h` is gitignored; copy `src/secrets.h.example` and fill in the
2.4GHz WiFi credentials, the Worker URL, the `DEVICE_TOKEN` you gave
`wrangler secret put`, and `DISPLAY_TITLE` (the header text — keep personal
names here, not in the repo). Placeholder values are detected and shown on
screen instead of fetch-looping.

## Build and flash

PlatformIO in a project venv (one-time: `python3 -m venv .venv &&
.venv/bin/pip install platformio` from the repo root), then:

```bash
cd firmware
../.venv/bin/pio run -t upload      # build + flash over USB
../.venv/bin/pio device monitor     # serial log, Ctrl-C to exit
```

or from the repo root, `scripts/update.sh device` (and `wifi` to edit
`secrets.h` first — the travel workflow). The log line
`balance fetch: state ok, heap N` confirms WiFi → NTP → TLS → auth → parse
all succeeded; failures print the TLS error string and heap figures.

Build pins that must not be casually upgraded: `espressif32@6.1.0` and M5GFX
0.1.11 (fetched from its GitHub tag — the registry dropped the 0.1.x series;
the demo's hand-rolled display driver targets that API).

## How the live fetch works (`src/test/app_balance.cpp`)

- **Verified TLS**: `WiFiClientSecure` validates Cloudflare's certificate
  against `src/ca_certs.h` — six public roots (Google Trust Services R1–R4,
  ISRG X1/X2). One NTP sync after WiFi join supplies the clock the check
  needs. If Cloudflare ever leaves both root families the device fails safe
  (stale screen) until the bundle is updated.
- **Async**: the fetch runs on its own 12KB FreeRTOS task, so buttons and the
  PWR restart keep working through the worst case ~35s of WiFi/NTP/HTTP
  timeouts. It writes only a staging struct; the UI loop copies it on
  completion — one writer per struct, no locks.
- **Cache**: the last `ok` payload is stored in NVS, so a cold boot shows the
  balance instantly while the first fetch runs. Degraded payloads are never
  cached.
- **Cadence**: every 60s on USB, every 5 min on battery, plus long-press A;
  waking from dim to data older than a minute forces a refresh.
- **Heap**: the framebuffer sprite is 8-bit (32KB instead of 65KB) so the
  ~50KB TLS handshake fits alongside it — with the 16-bit sprite it failed
  with "SSL - Memory allocation failed".

## Power policy (shared by every screen)

Full brightness while in use or on USB. On battery: dim after 20s idle,
deep-sleep after 2 min; the front button wakes to a fresh boot (cached
balance appears immediately). USB detection is a battery-voltage heuristic
(GPIO38) with smoothing and hysteresis (≥4.12V → USB, ≤4.04V → battery); a
freshly charged cell can read as "USB" for some minutes after unplugging.
Every misclassification is recoverable — the policy never calls
`power_off()`, which on USB never returns and hangs the device.

## Changes to the vendored demo

Everything under `src/test/` except `app_balance.cpp` is upstream code; these
are the deliberate edits (each with its rationale in the commit history):

- `test_wifi.cpp` — the WiFi-scan screen tore the radio down with raw
  `esp_wifi_deinit()`, which breaks the next `WiFi.begin()`; now
  `WiFi.mode(WIFI_OFF)`.
- `test_ble.cpp` — Bluedroid (~80–100KB) is released when the BLE screen
  exits (`deinit(false)`, so it still works on revisit).
- `test_lcd.cpp` — 8-bit framebuffer (see Heap above).
- `test_key.cpp` — the power-hold pad is latched through `esp_restart()`
  (otherwise a restart powers the board off); the PWR-hold message says what
  actually happens; `checkReboot()`/`checkNext()` drive the power policy.
- `test.cpp` — `balance_app()` runs first in the screen cycle; the hold latch
  is released after boot.
- `test.h` — declarations for the Balance app and power policy;
  `Preferences.h` is included here because PlatformIO's dependency finder
  misses includes added in `src/test/*.cpp`.
- `platformio.ini` — ArduinoJson; M5GFX pinned via tag tarball;
  `lib_ldf_mode = deep+`; `monitor_speed = 115200` (the default 9600 shows
  the log as garbage).
