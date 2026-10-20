# Upstream status

The exact state of every upstream thread involved, with links. Checked
2026-10-20.

## deskflow

### Issue #10166 — *"Wayland (Hyprland) server + macOS client: one wheel notch arrives as 16-17 whole clicks (~50 lines) — regression from the whole-click scroll change"*

- https://github.com/deskflow/deskflow/issues/10166
- **closed** (2026-10-13), but *not* actually fixed for this configuration.
- Reporter's environment matches this project's target exactly: CachyOS +
  deskflow 1.26.0.437 + Hyprland 0.56.2 + libei 1.6.0 + portal 1.4.1, macOS
  27.2 client, a mouse reporting both `REL_WHEEL` and `REL_WHEEL_HI_RES`.
- Their VERBOSE log is the smoking gun and is reproduced by `tools/simulate.py`:

  ```
  VERBOSE: event: scroll (0.00, -15.00)
  VERBOSE: event: scroll discrete (0, -15)
  VERBOSE: send mouse wheel to "…" +0,+120
  VERBOSE: event: scroll (0.00, -15.00)
  VERBOSE: event: scroll discrete (0, -15)
  VERBOSE: send mouse wheel to "…" +0,+2040      ← 17 clicks
  ```

- A fast flick reaches 81 clicks in a single event. They worked around it by
  dropping the client scale to 0.1.
- The issue was closed by PR #10257, which only addresses the macOS side.

### PR #10257 — *"Use the scroll modifier directly on mac os"*

- https://github.com/deskflow/deskflow/pull/10257
- **merged** 2026-10-13T21:18:15Z (sithlord48), +1/−3, one file:
  `src/lib/platform/OSXComputer.mm`.
- Removes the hardcoded 3× boost:

  ```diff
   void OSXComputer::fakeMouseWheel(ScrollDelta delta) const
   {
     if (delta.x != 0 || delta.y != 0) {
  -    // use server's acceleration with a little boost since other platforms
  -    // take one wheel step as a larger step than the mac does.
       delta = applyScrollModifier(
  -        {static_cast<int32_t>(3.0 * delta.x / s_scrollDelta), static_cast<int32_t>(3.0 * delta.y / s_scrollDelta)}
  +        {static_cast<int32_t>(delta.x / s_scrollDelta), static_cast<int32_t>(delta.y / s_scrollDelta)}
       );
  ```

- `fixes: #10254`, `fixes: #10166`. Tested only "Locally, Wayland → mac os (client)".
- **Not in Synergy 3.8.4** (core snapshot predates it).
- Note: this is the patch that fixes the *size* of the jump, not its
  *regularity*. 2040 units still arrives as 17 clicks — you just see 17 lines
  instead of 51. The shim is still needed to make notches uniform.

### Issue #10254 — *"macOS client scroll multiplier applied incorrectly"*

- https://github.com/deskflow/deskflow/issues/10254
- **closed** 2026-10-13 by PR #10257, milestone 1.28.0.

### PR #10277 — *"fix(wayland): separate smooth and discrete scroll remainders"*

- https://github.com/deskflow/deskflow/pull/10277
- **OPEN**, not merged, +15/−12, one file: `src/lib/platform/EiComputer.cpp`.
- Description, verbatim:

  > On a Wayland server receiving both smooth and discrete scroll events, the
  > handlers **share remainder values despite using different units**. This can
  > cause **oversized scroll steps** and scrolling in the old direction after
  > reversing the wheel.

- The patch only splits the accumulator:

  ```diff
  -struct ScrollRemainder { double x; double y; };
  +struct ScrollRemainder { double smoothX; double smoothY; double discreteX; double discreteY; };
  ```

  plus a comment stating the units (`smooth: 1.0 == one full 120-unit click`,
  `discrete: 120 == one full click`). It does **not** change the `0.1`/`120`
  constants or the truncation semantics.
- Tested on Arch/Hyprland server with a **Windows** client — *"macOS was not
  tested"*. Verified by replaying the paired events from #10166.
- Authored with OpenAI Codex assistance.

## Hyprland

### `CInputManager::onMouseWheel` — `axisValue120(e.axis, e.delta)`

- `src/managers/input/InputManager.cpp:994-999` in v0.56.2; the same shape is
  still in `main`.
- **No issue filed, no fix.** A GitHub issue search for
  `repo:hyprwm/Hyprland+axisValue120` returned `total_count: 0` on 2026-10-20.
- Intensively verified in the shipped 0.56.2 binary (the compositor is stripped,
  so the analysis used `objdump -d` on `/usr/bin/Hyprland`):

  ```
  CInputManager::onMouseWheel @0x6fe046
    0x6fe0ab  mov  NProtocols::inputCapture, ...
    0x6fe0c7  vmovsd 0x10(%rbx), %xmm0        ; e.delta           (15)
    0x6fe0cc  mov  0x8(%rbx), %esi             ; e.axis
    0x6fe0cf  call 0x876710                    ; ← CInputCaptureProtocol::axis(axis, delta)
    0x6fe0dc  mov  0x4(%rbx), %r8d             ; e.deltaDiscrete   (120)
    0x6fe0e8  test %r8d, %r8d
    0x6fe0eb  jne  0x6fe7f0                    ; discrete != 0 → skip value120
    ...
    0x6fe110  call 0x876780                    ; ← axisValue120(axis, (int)delta)
  ```

  and at the `jne` target:

  ```
  0x6fe7f0  vxorpd  %xmm1,%xmm1,%xmm1
  0x6fe7f4  vucomisd %xmm1,%xmm0             ; is delta == 0 ?
  0x6fe7f8  jp  0x6fe11d                     ; NaN → frame
  0x6fe7fe  jne 0x6fe11d                     ; delta != 0 → frame (no value120 at all)
  0x6fe804  call 0x8767e0                    ; axisStop(axis)
  ```

  The source reads more simply than the asm: because `e.source == 0` is true for
  real wheels, `axisValue120` is called **unconditionally** for them.
- Also note `CInputCaptureProtocol::axis()` truncates through `int`
  (`vcvttsd2si` then `vcvtsi2sd`) before calling `ei_device_scroll_delta()` — a
  third, smaller wart on the same path.

### `libei.h` contract

`/usr/include/libei-1.0/libei.h`:

- `ei_device_scroll_delta()` — "logical pixels"
- `ei_device_scroll_discrete()` — "120 is one detent"
- and a comment forbidding calling both for the same input event.

Hyprland violates both halves, which is what lets the deskflow
shared-accumulator bug (③) fire.

## Synergy

No public tracker entry found for this. Synergy 3.8.4's `synergy-core` is a
deskflow build (`synergy-core v3.8.4, based on Deskflow v1.26, protocol v1.8`),
so the fixes have to come from deskflow; Synergy would need to ship a core built
from a deskflow revision containing PR #10257 **and** #10277.

## What a real fix looks like

All three, in this order of impact:

1. **Hyprland:** `axisValue120(e.axis, e.deltaDiscrete)` and stop emitting both
   channels for one detent (respect the libei contract). — largest single win,
   and currently not even reported.
2. **deskflow:** merge PR #10277 (separate remainders). Ceases to matter if (1)
   is done, but it removes the class of bug rather than one instance.
3. **deskflow:** PR #10257 (already merged) drops the macOS 3× boost, which is a
   matter of default taste rather than correctness.

With (1) and (2) applied, `tools/simulate.py` predicts `1, 2, 1, 2, 1, 3, …`
lines per notch — much better, still not perfectly uniform, which is why the
shim scales from the other end.

If you file the Hyprland issue, the three-line summary is: *for
`WL_POINTER_AXIS_SOURCE_WHEEL` events, `InputManager.cpp:996` passes the degree
delta (15) to `axisValue120()` instead of `e.deltaDiscrete` (120), and it calls
both `axis()` and `axisValue120()` for a single detent, which libei forbids.*