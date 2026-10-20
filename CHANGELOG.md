# Changelog

All notable changes to this project. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is
[SemVer](https://semver.org/).

## [1.0.0] — 2026-10-20

First release. Fixes runaway / irregular wheel scrolling on a Wayland server
(Hyprland) with a macOS client, as diagnosed on Synergy 3.8.4 (deskflow v1.26)
+ Hyprland 0.56.2 + libei 1.6.0.

### Added

- `src/ei_scroll_fix.c` — `LD_PRELOAD` shim interposing the four
  `ei_event_scroll_get_*` accessors. Neutralises the discrete channel and
  divides wheel-notch deltas by `1.5` so exactly one 120-unit click is emitted
  per physical notch.
- Trackpad guard: the divisor is applied only to values within `1e-6` of a
  multiple of `15.0`, so arbitrary trackpad deltas pass through untouched
  (`EISFIX_WHEEL_ONLY`).
- Environment tunables: `EISFIX_SMOOTH_DIV`, `EISFIX_DROP_DISCRETE`,
  `EISFIX_WHEEL_ONLY`, `EISFIX_DISABLE`, `EISFIX_DEBUG`, `EISFIX_LOG`.
- Self-rotating decision log (`EISFIX_LOG`, rotates at 2 MiB) — necessary
  because `synergy-service` swallows the core's stderr.
- `scripts/install.sh` — builds, installs to `~/.local/lib`, writes a systemd
  drop-in, restarts the unit, and verifies `LD_PRELOAD` in
  `/proc/<pid>/environ` and the mapping in `/proc/<pid>/maps`. Includes a
  `LD_BIND_NOW=1 LD_DEBUG=bindings` self-check that proves the shim actually
  interposes the target binary before the service is touched.
- `scripts/uninstall.sh` — removes the drop-in and restarts; the `.so` is left
  in place and reported.
- `test/` — a stub libei plus 14 assertions, so `make test` works with no
  session, no Hyprland and no Synergy installed.
- `tools/simulate.py` — reproduces the whole chain's arithmetic
  (`120, 2040, 1920, …` before, `120, 120, 120, …` after; 1000-notch drift
  check; coalesced fast-scroll check).
- `docs/root-cause.md` — the four defects with disassembly (x86-64 and arm64),
  upstream source excerpts, and the derivation of the 3/51/48/51 line pattern.
- `docs/alternatives.md` — the other options (client scale, waiting for
  upstream, X11 server, Wayland client) and a table of everything ruled out
  with the evidence for each.
- `docs/upstream.md` — exact status of deskflow issues #10166 / #10254 and PRs
  #10257 / #10277, plus the unfiled Hyprland defect and what a real fix needs.
- `docs/troubleshooting.md` — step-by-step for "installed but nothing changed".
- CI: build + test on x86-64 and aarch64, `-Werror` build, exported-symbol
  count check, shellcheck.

### Verified

- 183 real scroll events from a physical wheel, all transformed correctly
  (`smooth ±15.000 → ±10.000`, `discrete ±15 → 0`), on the live session.
- 1000 notches → 1000 events → 120000 units, zero drift.
- Coalesced notches scale linearly: `-30 → -20`, `-90 → -60`.
- Direction reversal leaves no stale remainder in the wrong direction.
- Local (cursor-on-Linux) scrolling is unaffected: the shim hooks only the
  receive-side accessors; the send side (`EiScreen::fakeMouseWheel` →
  `ei_device_scroll_discrete()`) is untouched, and the round trip is exact.

### Known limitations

- Requires the core to start **without** privilege escalation; `sudo`/`pkexec`
  scrub `LD_PRELOAD`.
- glibc only (musl's loader has different `LD_PRELOAD` semantics for already
  resolved symbols).
- Defect ④ (the macOS client's 3× boost) is deliberately **not** removed: 120
  units → 3 lines is close to a native macOS notch. Users who prefer 1 line per
  notch should set `yScrollScale` to `0.34–0.66` on the Mac instead of
  patching it out.
- The fix is a workaround. The real fix belongs in Hyprland (pass
  `e.deltaDiscrete`, and stop emitting both channels) and deskflow (merge
  PR #10277).
