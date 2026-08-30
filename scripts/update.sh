#!/usr/bin/env bash
# Update helpers for the balance gadget. Usage:
#   scripts/update.sh worker   test + deploy the Cloudflare Worker
#   scripts/update.sh device   build + flash the firmware (device on USB)
#   scripts/update.sh wifi     edit device secrets (e.g. new WiFi), then flash
#   scripts/update.sh all      worker, then device
set -euo pipefail
cd "$(dirname "$0")/.."

PIO=".venv/bin/pio"

worker() {
  (cd worker && npm test && npx wrangler deploy)
}

device() {
  if [[ ! -x "$PIO" ]]; then
    echo "PlatformIO not found. One-time setup:" >&2
    echo "  python3 -m venv .venv && .venv/bin/pip install platformio" >&2
    exit 1
  fi
  if [[ ! -f firmware/src/secrets.h ]]; then
    echo "firmware/src/secrets.h missing: this would flash MOCK mode." >&2
    echo "Copy firmware/src/secrets.h.example and fill it in, or Ctrl-C now." >&2
    read -r -p "Flash mock anyway? [y/N] " reply
    [[ "$reply" == "y" ]] || exit 1
  elif grep -qE 'FILL_ME_IN|<your-subdomain>|<device token>' firmware/src/secrets.h; then
    echo "warning: secrets.h still contains template placeholders." >&2
  elif ! grep -qE '^#define WORKER_URL' firmware/src/secrets.h; then
    echo "note: WORKER_URL is not defined — this builds MOCK mode (no fetching)." >&2
  fi
  (cd firmware && "../$PIO" run -t upload)
  echo "Fingerprint: live builds report Flash ~85%, mock ~80%."
}

wifi() {
  if [[ ! -f firmware/src/secrets.h ]]; then
    cp firmware/src/secrets.h.example firmware/src/secrets.h
    echo "seeded firmware/src/secrets.h from the template" >&2
  fi
  "${EDITOR:-nano}" firmware/src/secrets.h
  device
}

case "${1:-}" in
  worker) worker ;;
  device) device ;;
  wifi)   wifi ;;
  all)    worker; device ;;
  *)      sed -n 's/^#   //p' "$0"; exit 1 ;;
esac
