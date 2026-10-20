# Root cause: how one wheel notch becomes 17 clicks

Everything here was obtained from the **shipped binaries and the exact upstream
sources**, not from guessing. Addresses are from:

| Binary | Build |
|---|---|
| `/opt/Synergy/synergy-core` v3.8.4 (based on Deskflow v1.26) | Linux x86_64 |
| `/Applications/Synergy.app/Contents/MacOS/synergy-core` | macOS (arm64), Synergy 3.8.4 |
| `/usr/bin/Hyprland` 0.56.2 (commit `efb5099`) | Linux x86_64, stripped |
| `/usr/lib/libeis.so.1` 1.6.0 | Linux x86_64 |

Reproduce with `objdump -d` / `otool -tvV`, or re-run the arithmetic with
`tools/simulate.py`.

## 1. What libinput reports

A single notch of a classic mouse wheel produces two things at once:

- `REL_WHEEL` / `REL_HWHEEL` with value `±1` (logical), and
- `REL_WHEEL_HI_RES` / `REL_HWHEEL_HI_RES` with value `±120` (120 = one detent).

aquamarine (`src/backend/Session.cpp:615-651`) turns that into an
`IPointer::SAxisEvent`:

```cpp
aqe.delta    = get_scroll_value();        // 15   (degrees)
aqe.discrete = get_scroll_value_v120();   // 120  (v120 units)
// and the source is WL_POINTER_AXIS_SOURCE_WHEEL == 0
```

## 2. Hyprland puts the wrong number in the wrong channel

`src/managers/input/InputManager.cpp:994-999` in v0.56.2 — verbatim:

```cpp
PROTO::inputCapture->axis(e.axis, e.delta);        // 15  → smooth channel
if (e.source == 0)
    PROTO::inputCapture->axisValue120(e.axis, e.delta);   // 15  → v120 channel (!)
else if (e.delta == 0)
    PROTO::inputCapture->axisStop(e.axis);
PROTO::inputCapture->frame();
```

Two independent defects in three lines:

1. **`e.delta` instead of `e.deltaDiscrete`.** The v120 channel is being handed
   a degree value. A real detent is `120`; it sends `15` — 8× too small.
2. **Both channels, same event.** libei's own header (`libei.h`) says
   `ei_device_scroll_delta()` and `ei_device_scroll_discrete()` must not be
   called for the same input event. (Hyprland's own source even comments that
   "if an application supports v120, it should ignore discrete anyways" —
   but deskflow handles both.)

`hypr/main` has the same shape, so this is current design, not a regression from
the user's config. No issue had been filed upstream when this was written
(`api.github.com/search/issues?q=repo:hyprwm/Hyprland+axisValue120` → 0 results).

## 3. libei: the values arrive, the semantics conflict

`include/libei.h`:

- `ei_device_scroll_delta()` — logical **pixels**.
- `ei_device_scroll_discrete()` — **multiples of 120**.

Handlers in deskflow:

```
EI_EVENT_SCROLL_DELTA      → EiScreen::onPointerScrollEvent
EI_EVENT_SCROLL_DISCRETE   → EiScreen::onPointerScrollDiscreteEvent
```

Both are connected, so one detent delivers `delta(15)` *and* `discrete(15)`.

## 4. deskflow's two handlers share one accumulator in two units

`src/lib/platform/EiScreen.cpp` (v1.26) / `EiComputer.cpp` (master). v1.26 first
(`v126_EiScreen.cpp:666-737`):

```cpp
// smooth
static const int s_pixelsPerWheelClick = 10;
const int s_pixelToWheelRatio = s_scrollDelta / s_pixelsPerWheelClick;   // 12
dx += remainder->x;
double rx = modf(dx, &x);                       // x = whole pixels
if (x != 0 || y != 0)
    sendEvent(PrimaryScreenWheel,
              WheelInfo::alloc((int32_t)-x * s_pixelToWheelRatio, ...)); // pixels × 12
remainder->x = rx;

// discrete — "libei and deskflow both use multiples of 120, forward as-is"
sendEvent(PrimaryScreenWheel, WheelInfo::alloc(-dx, -dy));
```

`remainder` is `(double*)ei_device_get_user_data(device)` — **one** pair of
doubles for the whole device, written by *both* handlers while they interpret it
in *different units*:

| Handler | Unit stored in `remainder` |
|---|---|
| smooth | pixels (v1.26) / fractional clicks (master, `1.0 == one click`) |
| discrete | v120 units (`120 == one click`) |

The disassembly confirms they share the storage. In the 3.8.4 binary:

```
onPointerScrollDiscreteEvent  @0x107a70
  call ei_event_scroll_get_discrete_dx        ; r12d
  call ei_device_get_user_data                ; rax
  movsd 0x9fa2f(%rip), %xmm2                  ; = 120.0  (file offset 0x1a7510)
  cvtsi2sd %r12d,%xmm1 ; addsd (%rax),%xmm1   ; ← reads slot 0
  cvtsi2sd %ebp,%xmm0  ; addsd 0x8(%rax),%xmm0; ← reads slot 8
  divsd %xmm2,%xmm3 ; cvttsd2si %xmm3,%edi    ; trunc(sum/120)
  imul  $0x78,%edi,%edi                       ; ×120
  cvtsi2sd %edi,%xmm2 ; subsd %xmm2,%xmm1
  movsd %xmm1,(%rax)                          ; ← writes slot 0 back
```

and the smooth handler uses the *same* `(%rax)` / `0x8(%rax)` pair with a `0.1`
constant (`0x1a74c0`) and `trunc` via `andpd`+`cvttsd2si`. Master made the two
constants explicit (`0.1` and `120`) but kept the shared struct — that is
exactly what PR [#10277](https://github.com/deskflow/deskflow/pull/10277) is
about, and it is still open.

## 5. The arithmetic that produces 3 / 51 / 48 / 51 / 48

Master's formulas (identical in effect to the 3.8.4 disassembly):

```
smooth:    a = rem + dy*0.1 ;   t = trunc(a) ;  if t: emit(-t*120) ;  rem = a - t
discrete:  b = rem + d     ;   c = trunc(b/120)*120 ; if c: emit(-c) ; rem = b - c
```

Start with `rem = 0`, one notch gives `dy = 15`, `d = 15`
(the sign convention flips on the wire; magnitudes are what matter):

| notch | after smooth | emitted | rem | after discrete | emitted | rem |
|---|---|---|---|---|---|---|
| 1 | 1.5 | −1·120 = **120** | 0.5 | 0.5+15 = 15.5 | 0 | 15.5 |
| 2 | 15.5+1.5 = 17.0 | −17·120 = **2040** | 0.0 | 0+15 = 15 | 0 | 15 |
| 3 | 15+1.5 = 16.5 | −16·120 = **1920** | 0.5 | 15.5 | 0 | 15.5 |
| 4 | 17.0 | **2040** | 0.0 | 15 | 0 | 15 |

So the wire carries `120, 2040, 1920, 2040, 1920, …` — 1 then 16-17 clicks.

These are *literally* the numbers in upstream issue
[#10166](https://github.com/deskflow/deskflow/issues/10166):

```
VERBOSE: send mouse wheel to "…" +0,+120
VERBOSE: send mouse wheel to "…" +0,+2040
```

## 6. The macOS client multiplies by 3

`OSXScreen::fakeMouseWheel` @`0x10002eb30` (arm64):

```asm
lsr  x8, x1, #32          ; delta.y
; ...
fmov d1, #3.0
scvtf d0, x8
fmul  d0, d0, d1          ; 3.0 * delta.y
fmov  d1, #120.0
fdiv  d0, d0, d1          ; / 120
fcvtzs w8, d0             ; → lines
; × YScrollScale, apply invert flag
bl    _CGEventCreateScrollWheelEvent   ; unit = 1 = kCGScrollEventUnitLine
bl    _CGEventPost                     ; kCGHIDEventTap
```

Source (`src/lib/platform/OSXScreen.mm:588-604` in v1.26) still carries the
comment and the multiplier:

```cpp
// use server's acceleration with a little boost since other platforms
// take one wheel step as a larger step than the mac does.
delta = ... { static_cast<int32_t>(3.0 * delta.x / s_scrollDelta),
              static_cast<int32_t>(3.0 * delta.y / s_scrollDelta) };
```

So `120 → 3 lines`, `2040 → 51 lines`, `1920 → 48 lines`. That is the user-visible
`3, 51, 48, 51, 48, …`.

## 7. Why nothing on either machine can be configured around it

- **`OSXScreen::resetOptions()` @0x10002fb50 and `setOptions()` @0x10002fb54 are
  both a bare `ret`** — the macOS client ignores every option the server sends,
  including scroll scaling. The scales it *does* apply are read once, at
  construction, from local settings.
- **The protocol cannot carry fractional scrolls.** `kMsgDMouseWheel` is
  `"DMWM%2i%2i"` and `ProtocolUtil::writef` supports only `%i %I %s %S %%` —
  there is no float formatter in the protocol layer at all.
- **`scroll_sensitivity` (0.5 in both `db.json`s) is dead**: zero references in
  `synergy-core` and `synergy-service`; the service's type definition marks it
  `@deprecated Never read`. Only the Electron front-end mentions it.
- **`com.apple.scrollwheel.scaling`** (0.75 here) has **zero xrefs** in the macOS
  binary; only the generic `CFPreferencesCopyValue` in
  `mapScrollWheelToDeskflow` reads a preference, and that function is the
  *server* direction.
- **`fakeMouseWheel` imports only `CGEventCreateScrollWheelEvent`** — no
  `...Event2`, no `kCGScrollEventUnitPixel`, no phase/momentum API. There is no
  smoothing or interpolation anywhere in the path; it is line-unit integer steps
  or nothing.

## 8. Why the shim's approach is sufficient

Only two values enter deskflow from this bug: the smooth delta (`15`) and the
discrete delta (`15`). Interposing the four accessors lets us

1. return `0` for the discrete pair → the discrete handler accumulates `0`, so
   it writes back an unchanged remainder and never emits. The shared-accumulator
   corruption disappears because only one writer remains.
2. return `delta / 1.5` for the smooth pair → `15/1.5 = 10`, and
   `10 × 0.1 = 1.0`, so `trunc` yields exactly one click per notch.

Both are exact in IEEE-754: `10 * 0.1 == 1.0` is true, so 1000 notches give
1000 events and 120000 units with no drift (`tools/simulate.py`).

Coalesced notches behave linearly: `30 → 20 → 2 clicks`, `90 → 60 → 6 clicks`.

Defect ④ (the 3× boost) remains, and is *wanted* here: 120 units → 3 lines is
close to a native macOS notch. Removing it as well (i.e. also applying PR
#10257) would give 1 line per notch, which most users find too slow — that is a
matter of taste, and the client's `yScrollScale` handles it without a rebuild.
