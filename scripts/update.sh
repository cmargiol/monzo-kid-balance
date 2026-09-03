#!/usr/bin/env bash
# Everyday commands for the balance gadget. Run with no arguments for help.
set -euo pipefail
cd "$(dirname "$0")/.."

PIO=".venv/bin/pio"

usage() {
  cat <<'HELP'
scripts/update.sh — the only commands you need after setup

  scripts/update.sh worker    Test and deploy the Cloudflare Worker.
  scripts/update.sh device    Build the firmware and flash it over USB.
  scripts/update.sh wifi      Change WiFi (opens secrets.h), then flash.
  scripts/update.sh all       Worker, then device.
  scripts/update.sh --help    This text.

Plug the M5Stick in over USB-C for device and wifi. If a step is going to
flash the mock build (demo data, no secrets.h), it asks first.

One-time tooling: Node (for npx wrangler) and PlatformIO in a project venv:
  python3 -m venv .venv && .venv/bin/pip install platformio
HELP
}

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
    echo "firmware/src/secrets.h missing: this would flash the MOCK build (demo data)." >&2
    echo "Copy firmware/src/secrets.h.example and fill it in, or answer y to" >&2
    echo "flash the mock build (handy for screenshots and trying it out)." >&2
    read -r -p "Flash mock build? [y/N] " reply
    [[ "$reply" == "y" ]] || exit 1
  elif grep -qE 'FILL_ME_IN|<your-subdomain>|<device token>' firmware/src/secrets.h; then
    echo "warning: secrets.h still contains template placeholders." >&2
  elif ! grep -qE '^#define WORKER_URL' firmware/src/secrets.h; then
    echo "note: WORKER_URL is not defined — this builds the MOCK version (no fetching)." >&2
  fi
  (cd firmware && "../$PIO" run -t upload)
  echo "Done. (Live builds report Flash ~62%, mock builds ~43%.)"
}

wifi() {
  if [[ ! -f firmware/src/secrets.h ]]; then
    cp firmware/src/secrets.h.example firmware/src/secrets.h
    echo "created firmware/src/secrets.h from the template" >&2
  fi
  "${EDITOR:-nano}" firmware/src/secrets.h
  device
}

case "${1:-}" in
  worker)          worker ;;
  device)          device ;;
  wifi)            wifi ;;
  all)             worker; device ;;
  ""|-h|--help|help) usage ;;
  *)               echo "unknown command: $1" >&2; echo >&2; usage >&2; exit 1 ;;
esac
