# ei-scroll-fix

[![build](https://github.com/pingod/ei-scroll-fix/actions/workflows/build.yml/badge.svg)](https://github.com/pingod/ei-scroll-fix/actions/workflows/build.yml)

**Fix jerky / runaway wheel scrolling when Synergy or Deskflow runs as a Wayland
server and a macOS client receives the scroll.**

One physical wheel notch should scroll a few lines. On a Wayland server it can
arrive at the client as **16–17 whole clicks (~50 lines)**, and consecutive
notches alternate between 1 click and 17 clicks — the scroll feels like it
stutters or bolts away from you. This project explains why, and ships a
10 KB `LD_PRELOAD` shim that fixes it on the server side without rebuilding
anything.

```
per notch on macOS:   3 lines → 51 → 48 → 51 → 48 → …      (broken)
after the fix:        3 lines →  3 →  3 →  3 →  3 → …      (fixed)
```

## Is this my problem?

All of these must hold:

- Server runs **Wayland** (the core uses the input-capture portal + libei).
  Check: `echo $XDG_SESSION_TYPE` → `wayland`.
- Server is **deskflow** or **Synergy 3.x** (Synergy 3.8.4 embeds deskflow v1.26).
- Client is **macOS** (`OSXComputer::fakeMouseWheel` has a hardcoded 3× boost).
- Mouse (not trackpad) scrolling feels too fast *and* irregular — the
  "one notch = 1 click, next notch = 17 clicks" pattern is the giveaway.

You can confirm it in one command, without installing anything — see
[Verifying the diagnosis](#verifying-the-diagnosis).

## Quick start

```bash
git clone https://github.com/pingod/ei-scroll-fix
cd ei-scroll-fix
make test                                  # 14 assertions, no session needed
./scripts/install.sh                       # builds, installs, restarts the unit
```

Then move the pointer to the macOS screen and scroll once:

```bash
tail -3 ~/.local/state/Synergy/scroll-fix.log
```

```
1792485587.516 [107731] active smooth_div=1.5000 drop_discrete=1 wheel_only=1 disabled=0
1792485587.516 [107731] wheel   dy 15.000 -> 10.000
1792485587.516 [107731] discrete dy 15 -> 0
```

Every notch must show `dy ±15.000 -> ±10.000` and `discrete ±15 -> 0`.
Undo with `./scripts/uninstall.sh`.

## What was actually wrong

Four independent defects line up on this one code path. Fixing any single one
is not enough — the shim has to neutralise the two that live on the server.

```
physical wheel detent              1 notch = 15°  and  value120 = 120
   │
   ├─ aquamarine  Session.cpp:615-651
   │      aqe.delta    = get_scroll_value()       → 15   (degrees)
   │      aqe.discrete = get_scroll_value_v120()  → 120  (v120)
   ▼
Hyprland  InputManager.cpp:994-999        CInputManager::onMouseWheel
   │   PROTO::inputCapture->axis(e.axis, e.delta);            // 15  ─┐
   │   if (e.source == 0)                                          │ ① wrong value (should be
   │       PROTO::inputCapture->axisValue120(e.axis, e.delta); // 15 ─┘    deltaDiscrete = 120)
   │                                                               ② both channels for one event
   ▼
libei  ei_device_scroll_delta(15)  +  ei_device_scroll_discrete(15)
   ▼
deskflow / Synergy  EiScreen  (PR #10277 still open)
   │   onPointerScrollEvent:          acc += dx * 0.1;  trunc;     // unit = clicks (1.0 = 1 notch)
   │   onPointerScrollDiscreteEvent:  acc += dx;        / 120.0;   // unit = v120 (120 = 1 notch)
   │   → ③ both handlers share ONE remainder pair from ei_device_get_user_data
   ▼
protocol  DMWM%2i%2i   (int16, whole 120-unit clicks — no fractional variant exists)
   ▼
macOS client  OSXComputer::fakeMouseWheel
       (int)(y * 3.0 / 120.0)   ← ④ hardcoded 3× boost, removed upstream by PR #10257
       CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 2, y, x)
```

| # | Where | Defect | Fixed upstream? |
|---|---|---|---|
| ① | Hyprland `InputManager.cpp:996` | `axisValue120(e.axis, e.delta)` passes **degrees** (15) into a channel that wants **v120** (120) | **no** — no issue filed |
| ② | Hyprland, same lines | emits *both* smooth and discrete for one detent; libei's header explicitly forbids that | **no** |
| ③ | deskflow `EiComputer.cpp` | the two scroll handlers share one remainder pair **in two different units** — a discrete write poisons the fractional-click accumulator | PR [#10277](https://github.com/deskflow/deskflow/pull/10277) — **open** |
| ④ | deskflow `OSXComputer.mm` | `3.0 *` boost in the macOS client | PR [#10257](https://github.com/deskflow/deskflow/pull/10257) — **merged** 2026-10-13 |

The arithmetic that follows from ③: the shared accumulator drifts, so the
truncation alternates between 1 and 17 clicks. Multiply by ④'s 3× and you get
3, 51, 48, 51, 48 lines per notch. Full derivation with the real binary
expressions in [`docs/root-cause.md`](docs/root-cause.md).

## How the fix works

`src/ei_scroll_fix.c` interposes two libei accessors — the only place where the
bad values enter deskflow:

| Intercepted symbol | Normal return | Shim returns | Why |
|---|---|---|---|
| `ei_event_scroll_get_discrete_dx/dy` | `15` | **`0`** | kills defect ①/②: the discrete channel carries a value 8× too small, so drop it entirely |
| `ei_event_scroll_get_dx/dy` | `15.0` | **`10.0`** (`÷1.5`) | 10 "pixels" × `0.1` = exactly `1.0` click → exactly one 120-unit event per notch |

Result: every notch becomes a uniform 120 units, i.e. a uniform 3 lines per
notch on macOS — which is what defect ④'s boost was originally trying to
approximate.

### Why dividing by 1.5 is the right constant

deskflow's smooth handler is `acc += dx * 0.1` and it emits when
`trunc(acc) != 0`, so **10 units = 1 notch**. A physical detent arrives as
`15.0` on the EIS path (Hyprland forwards libinput's angle untouched). The
divisor is therefore `15 / 10 = 1.5`, and it is exact: `10 * 0.1 == 1.0` holds
in IEEE-754 double, so 1000 notches produce exactly 1000 events with zero drift
(`make test` + `tools/simulate.py` both check this).

### Why it does not break the trackpad

A wheel detent is an **exact multiple of `15.0`**; a trackpad delta is an
arbitrary float. The divisor is applied only when the value is within `1e-6` of
a multiple of 15:

```
  15.000  wheel    → ÷1.5 →  10.000      ⤷ scaled
  30.000  wheel    → ÷1.5 →  20.000      ⤷ scaled (2 notches coalesced)
   7.300  trackpad → untouched →  7.300
 -13.700  trackpad → untouched → -13.700
  15.010  edge case → untouched → 15.010   (not an exact multiple)
```

Set `EISFIX_WHEEL_ONLY=0` to divide unconditionally (this *will* slow the
trackpad down).

### Why it does not break scrolling on the Linux screen

When the pointer is on the Linux machine deskflow *sends* scroll via
`EiScreen::fakeMouseWheel`, which calls `ei_device_scroll_discrete()` — the
send side. The shim only touches the four `ei_event_scroll_get_*` receive-side
accessors, so the local path is untouched. Verified round trip: 1 physical
notch → 120 units → exactly 1 notch locally.

## Verifying the diagnosis

Before installing anything you can prove which of the four defects you have.

**1. Ask the server what it is about to send.** The relevant log lines are
`LOG_VERBOSE` (`%z5`), and the Synergy service *downgrades* `VERBOSE` to `DEBUG`
in production builds — so the GUI cannot show them and neither can
`[log] level=DEBUG`. You must run the core by hand:

```bash
systemctl --user stop synergy-3.service
/opt/Synergy/synergy-core server --settings ~/.config/Synergy/synergy-core.conf
# scroll a few notches, then:
#   event: scroll (0.00, -15.00)          ← smooth channel, 15 degrees
#   event: scroll discrete (0, -15)       ← discrete channel, 15 (!) not 120
#   send mouse wheel to "mac…" +0,+120
#   send mouse wheel to "mac…" +0,+2040   ← 17 clicks
```

**2. Confirm the client multiplies by 3.** On the Mac:

```bash
otool -tvV /Applications/Synergy.app/Contents/MacOS/synergy-core \
  | awk '/fakeMouseWheel/,/ret/' | grep -E 'fmul|fcvt'
```

**3. Rule out everything else** — see [`docs/alternatives.md`](docs/alternatives.md).
Short version: it is not the network, not `input:scroll_factor`, not
`emulate_discrete_scroll`, not `scroll_sensitivity` (a dead field with zero
references in the binaries), not `com.apple.scrollwheel.scaling` (zero xrefs),
and not high-resolution wheel hardware.

## Tuning

| Variable | Default | Meaning |
|---|---|---|
| `EISFIX_SMOOTH_DIV` | `1.5` | Divisor for wheel-notch deltas. `1.5` = exactly one notch per notch. **Lower** = faster (`1.2` ≈ 3.75 lines, `1.0` = no scaling ≈ 4-5 lines); **higher** = stuttery, not slower |
| `EISFIX_DROP_DISCRETE` | `1` | `0` keeps Hyprland's broken channel (scroll gets bad again) |
| `EISFIX_WHEEL_ONLY` | `1` | `0` divides unconditionally, **slowing the trackpad** |
| `EISFIX_DISABLE` | — | `1` = behave as if not installed |
| `EISFIX_DEBUG` | — | `1` = print decisions to stderr (useful only for a hand-run core) |
| `EISFIX_LOG` | — | append decisions to this file; survives the service's stderr pipe |

> Raising `EISFIX_SMOOTH_DIV` above `1.5` makes a single notch accumulate to
> less than one click, which *feels* notchy rather than slow. To scroll slower,
> raise the client's scale instead (below).

Once the fix is in place the client's scale becomes a clean integer multiplier
instead of a truncation lottery:

| macOS `yScrollScale` | lines per notch |
|---|---|
| 0.34 – 0.66 | 1 |
| 0.67 – 0.99 | 2 |
| 1.0 (default) | 3 |
| 1.34 – 1.66 | 4 |
| 2.0 | 6 |

## Requirements

- Linux server, `x86_64` or `aarch64`, glibc.
- `gcc`/`clang` and `make` (only to build; the result is one `.so`).
- The server core must launch **without privilege escalation** — `sudo`/`pkexec`
  scrubs `LD_PRELOAD`. Synergy's `elevate=true` does *not* escalate on Linux
  (verified: the log file it writes is owned by the user, not root). If your
  setup does escalate, use the manual method below instead.

## Manual / non-systemd use

```bash
make
LD_PRELOAD=$PWD/build/ei_scroll_fix.so EISFIX_DEBUG=1 \
  /opt/Synergy/synergy-core server --settings ~/.config/Synergy/synergy-core.conf
```

For any other launcher, export the variables in the environment that finally
`exec`s the core.

## Uninstall

```bash
./scripts/uninstall.sh            # restores the original scroll behaviour
rm -f ~/.local/lib/ei_scroll_fix.so
```

Nothing outside `~/.local/lib`, `~/.config/systemd/user/synergy-3.service.d`
and `~/.local/state/Synergy/scroll-fix.log` is modified. `/opt/Synergy` and all
Synergy/Hyprland configuration files are left alone.

## Documentation

- [`docs/root-cause.md`](docs/root-cause.md) — the full chain with disassembly
  and source evidence, and the arithmetic that produces 3/51/48/51
- [`docs/alternatives.md`](docs/alternatives.md) — what else you can try, what
  does not work, and what was ruled out
- [`docs/upstream.md`](docs/upstream.md) — the upstream issues/PRs, their exact
  status, and what a real fix looks like
- [`docs/troubleshooting.md`](docs/troubleshooting.md) — the shim is installed
  but nothing changed; where to look
- [`CHANGELOG.md`](CHANGELOG.md)

## License

MIT — see [`LICENSE`](LICENSE). This project contains no code from deskflow,
Synergy or Hyprland; it only interposes symbols exported by libei at runtime.
See [`NOTICE`](NOTICE) for upstream references.

---

中文文档：[README.zh-CN.md](README.zh-CN.md)
