# Alternatives, and what was ruled out

Ordered by how much I recommend them. Option 1 is what this repo does.

## 1. Server-side `LD_PRELOAD` shim (this project)

- **Cost:** one `.so` + one systemd drop-in; no rebuild, no patch, no config edit.
- **Result:** uniform 120 units per notch → uniform 3 lines per notch.
- **Risk:** low. Only the four `ei_event_scroll_get_*` accessors are touched;
  the trackpad is protected by the exact-multiple-of-15 guard; the local
  (cursor-on-Linux) path is a different code path entirely.
- **Undo:** `./scripts/uninstall.sh`.

## 2. Lower the macOS client's scroll scale (no code, but lossy)

GUI: *screen settings → this screen → Scroll speed*. Underlying keys are
`[client] yScrollScale` / `xScrollScale` in
`~/Library/Preferences/Synergy/synergy-core.conf`.

This is applied **after** `÷120`, then truncated to `int32`, so a small scale
does not scale the jitter down — it deletes whole clicks:

| scale | lines per notch | problem |
|---|---|---|
| 1.0 | `3, 51, 48, 51, …` | the bug |
| 0.5 | `1, 25, 24, …` | still 25× |
| 0.1 | `0, 5, 4, 5, …` | **first notch lost**, then 4-5 lines |
| 0.05 | `0, 2, 2, 2, …` | first notch lost |

It trades amplitude for dropped notches. The reporter of upstream #10166 lived
with `0.1` for a while; it is usable but it never fixes the 25× jitter.

**After** applying the shim, this knob becomes a clean integer multiplier and is
the recommended way to fine-tune speed — see the table in the README.

## 3. Wait for / backport the upstream patches

| Patch | Status | Effect |
|---|---|---|
| deskflow PR [#10277](https://github.com/deskflow/deskflow/pull/10277) — separate smooth/discrete remainders | **open** | fixes defect ③; on its own still leaves 3/6/3/6… because Hyprland's 15-instead-of-120 remains |
| deskflow PR [#10257](https://github.com/deskflow/deskflow/pull/10257) — drop the macOS 3× boost | **merged** 2026-10-13 | fixes defect ④ |
| Hyprland `axisValue120(e.axis, e.delta)` → `e.deltaDiscrete` | **not fixed, no issue filed** | fixes defects ① and ②; alone would make the discrete channel correct, but deskflow still double-counts both channels |

Synergy 3.8.4's core is a deskflow snapshot from 2026-09-15..09-29, so it has
neither PR. Even with both merged, one Hyprland-side defect remains, and — as
`tools/simulate.py` shows — the result is `1, 2, 1, 2, 1, 3, …` lines per notch:
better, but still not uniform. The shim is the only option that fixes the
whole chain today.

## 4. Switch the server to an X11 session

`ServerApp::createScreen()` picks the backend from `XDG_SESSION_TYPE`: exactly
`"wayland"` (length 7) → `EiScreen`, anything else → `XWindowsScreen`. Both
implementations are in the binary.

Not recommended under Hyprland: you would only get XWayland, where global
grabbing of physical wheel events is unreliable, and you lose the rest of the
Wayland session.

## 5. Use a Wayland-native client instead of the macOS one

Only worth mentioning because it sidesteps all four defects: with a Linux or
Windows client the 3× boost (④) is absent. Defects ①②③ still apply, so expect
`1, 17, 16, 17, …` clicks — shifted, not fixed.

## Ruled out — do not waste time here

| Candidate | Why it is not the cause |
|---|---|
| **Network / Wi-Fi** | `ping` Linux→mac: 0% loss, rtt 0.305–0.604 ms, mdev 0.088 ms. Scroll events are ~1 per notch; latency cannot reorder or multiply them. |
| **`input:scroll_factor`** (Hyprland) | Read at `0x6fe5d4-0x6fe651`, *after* the libei forwarding at `0x6fe0cf`, and only used on the `CSeatManager::sendPointerAxis` path (Wayland clients). Does not touch the EIS path at all. |
| **`input:emulate_discrete_scroll`** | Its accumulator (InputManager.cpp:1046-1077) only feeds `sendPointerAxis` for Wayland clients. Also does not touch the EIS path. |
| **`scroll_sensitivity`** (`0.5` in both `db.json`s) | Zero string references in `synergy-core` and `synergy-service`. The service's type marks it `@deprecated Never read`. Dead field from an older UI. |
| **`invert_scroll` / `x_scroll_scale` / `y_scroll_scale` in `db.json`** | Server-side copies are not transmitted: `OSXScreen::setOptions()` is a bare `ret`. Only the client's *local* values matter. |
| **`com.apple.scrollwheel.scaling`** (0.75) | Zero xrefs in the macOS binary for the client direction; the only reader is `mapScrollWheelToDeskflow` (server direction). |
| **High-resolution wheel hardware** | All four input devices on this machine have `REL_WHEEL_HI_RES` (bit 11) and `REL_HWHEEL_HI_RES` (bit 12) **unset** in `/sys/class/input/event*/device/capabilities/rel` — the kernel never generates them here, so that is not a factor. (Issue #10166's reporter *did* have them; the bug reproduces either way.) |
| **The 1131 `can't get the active group` WARNINGs** | At `0x1000286d0`, in the low-level HID/group query path. Fires per input event, unrelated to scroll semantics. Cosmetic. |
| **`[log] level=DEBUG` showing no wheel lines** | Expected: the wheel lines are `LOG_VERBOSE` (`%z5`) and `synergy-service` clamps `VERBOSE` → `DEBUG` in production builds (`effectiveLogLevel(level, isConfigDevelopment)`). You must run the core by hand to see them. |
| **Protocol version 1.8** | v1.8 is about keyboard language codes / key repeat / language sync. Scroll is `DMWM%2i%2i` and unchanged. |
| **Reinstalling / restarting Synergy** | The reconnect storm in the logs (108 retries at 06:3x) happened during a different time window than the reported stutter, and the arithmetic reproduces deterministically with a stable connection. |
