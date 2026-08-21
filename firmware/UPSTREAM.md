# Vendored from m5stack/M5StickCPlus2-UserDemo

This directory started as a verbatim copy of M5Stack's factory demo firmware:

- **Source:** <https://github.com/m5stack/M5StickCPlus2-UserDemo>
- **Commit:** `3d3d9da` (repo HEAD, Dec 2023 — the repo is effectively frozen)
- **License:** MIT (see `LICENSE` in this directory)

Why vendored rather than a submodule: upstream has 4 commits and no activity,
so there is nothing to track — and we deliberately modify it (see git history
of this directory for every change since the verbatim import commit).

The factory firmware can always be restored to the device with M5Burner:
<https://docs.m5stack.com/en/guide/restore_factory/m5stickc_plus2>
