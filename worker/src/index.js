/**
 * monzo-kid-balance Worker.
 *
 * Serves a pre-formatted, read-only "what the gadget should show" payload.
 * The device never talks to Monzo; this Worker is the only thing that will
 * (in later PRs). For now /display returns a hardcoded mock so the endpoint
 * shape, auth, and deployment can be verified end to end.
 */

// Payload contract with the firmware. `state` is one of:
//   ok           — show balance/tx as given
//   needs_reauth — show the "ask a grown-up to re-approve" screen
//   error        — show last-good data with a "stale" badge
// Strings are pre-formatted server-side; the device does zero money math.
const MOCK_DISPLAY = {
  state: "ok",
  balance: "£12.34",
  today: "£1.20 spent today",
  updated: "12:00",
  tx: [
    ["Toy Shop", "-£4.99"],
    ["Pocket money", "+£2.00"],
    ["Bus", "-£1.75"],
  ],
};

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === "/display" && request.method === "GET") {
      if (!(await isDevice(request, env))) {
        return json({ error: "unauthorized" }, 401);
      }
      return json(MOCK_DISPLAY);
    }

    return json({ error: "not found" }, 404);
  },
};

/** Bearer-token check for the gadget, constant-time to avoid oracle timing. */
async function isDevice(request, env) {
  if (!env.DEVICE_TOKEN) return false; // secret not configured yet
  const auth = request.headers.get("Authorization") ?? "";
  const token = auth.startsWith("Bearer ") ? auth.slice(7) : "";
  return timingSafeEqual(token, env.DEVICE_TOKEN);
}

function timingSafeEqual(a, b) {
  const enc = new TextEncoder();
  const ab = enc.encode(a);
  const bb = enc.encode(b);
  if (ab.byteLength !== bb.byteLength) return false;
  return crypto.subtle.timingSafeEqual(ab, bb);
}

function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
    },
  });
}
