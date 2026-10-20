# Troubleshooting

The common case — "installed it, scrolled, nothing changed" — has one dominant
cause: **the shim is not in the process.** Work down this list in order.

## 0. Is the service actually running the core?

```bash
systemctl --user status synergy-3.service --no-pager | head -20
pgrep -a synergy-core
```

Synergy 3 starts the core as a *child* of `synergy-service`, not as its own
unit, so `pgrep` is the reliable check:

```
98605 /opt/Synergy/synergy-core server --settings /home/you/.config/Synergy/synergy-core.conf
```

If there is no `synergy-core` process, nothing can work. Start it with
`systemctl --user restart synergy-3.service`.

## 1. Did the environment reach the core?

`LD_PRELOAD` must be in the core's environment, not in the service's.

```bash
pid=$(pgrep -x synergy-core | head -1)
tr '\0' '\n' < /proc/$pid/environ | grep -E 'LD_PRELOAD|EISFIX'
```

Expected:

```
LD_PRELOAD=/home/you/.local/lib/ei_scroll_fix.so
EISFIX_SMOOTH_DIV=1.5
EISFIX_DROP_DISCRETE=1
EISFIX_WHEEL_ONLY=1
EISFIX_LOG=/home/you/.local/state/Synergy/scroll-fix.log
```

Missing → the drop-in is not being read. Check that it is exactly at
`~/.config/systemd/user/synergy-3.service.d/scroll-fix.conf`, then
`systemctl --user daemon-reload` **followed by** a restart (a reload alone does
not re-exec a running unit).

## 2. Is the library mapped into the process?

```bash
grep ei_scroll_fix /proc/$pid/maps
```

Three lines (r--p, r-xp, r--p). Nothing → the path in `LD_PRELOAD` is wrong, or
the loader refused it. Confirm the file exists and is world-readable:

```bash
ls -l ~/.local/lib/ei_scroll_fix.so    # 644 or 755
```

A `LD_PRELOAD` pointing at a 0600 file owned by another user is silently
ignored.

## 3. Was the process started through something that scrubs the environment?

Anything that goes through `sudo`, `pkexec`, `su`, or a setuid helper loses
`LD_PRELOAD` (the dynamic loader drops it for setuid binaries).

Synergy's `[daemon] elevate=true` looks alarming but does **not** escalate on
Linux — the escalation code path is macOS-only. Verify for your install: the
core's log file (`~/.local/state/Synergy/synergy.jsonl`) should be owned by
*you*:

```bash
ls -l ~/.local/state/Synergy/synergy.jsonl
```

If it is owned by `root`, the core is privileged and `LD_PRELOAD` cannot work.
Use the "run the core by hand" path in the README instead, or launch the core
from a wrapper that is not setuid.

## 4. Does the log file exist at all?

The shim is **lazy**: it opens the log on the first intercepted call, which is
the first scroll event. So:

- no scroll yet → no file. Scroll once over the macOS screen.
- file exists but the last lines are old → the scroll is being sent to a
  *different* computer (check which screen the pointer is on), or the pointer is
  on the Linux screen, where a different code path runs and the shim is never
  called.

```bash
ls -l ~/.local/state/Synergy/scroll-fix.log
tail -5 ~/.local/state/Synergy/scroll-fix.log
```

Good output:

```
1792485587.516 [107731] active smooth_div=1.5000 drop_discrete=1 wheel_only=1 disabled=0 ...
1792485600.114 [107731] wheel   dy 15.000 -> 10.000
1792485600.114 [107731] discrete dy 15 -> 0
```

Every notch shows `±15.000 -> ±10.000`. If you instead see

```
smooth dy 7.300 passthrough (continuous)
```

that is the trackpad guard doing its job — a wheel should never take that
branch.

## 5. The log says the right thing but the Mac still feels wrong

Then the server did its job and the problem is on the client side or is simply
the 3× boost being larger than you want. Options:

- Lower `yScrollScale` on the Mac (see the tuning table in the README — after
  the fix it is a clean integer multiplier: `0.34–0.66` → 1 line,
  `0.67–0.99` → 2 lines).
- The macOS client ignores server-pushed options (`OSXScreen::setOptions()` is
  an empty function in 3.8.4), so this **must** be set locally, on the Mac.

Check what the Mac actually received:

```bash
# on the Mac
defaults read com.symless.synergy3 2>/dev/null | grep -i scroll
```

## 6. It worked, then stopped after a reboot

The drop-in and the `.so` both live in `$HOME`, so they survive. What does not
survive is `~/.local/lib` being cleaned by something, or the unit being
reinstalled by an app update (which can drop `synergy-3.service.d/` contents).

Re-run `./scripts/install.sh` — it is idempotent, and it keeps an existing
`override.conf` next to it.

## 7. Verifying without a Mac

You can test the whole thing on the Linux box alone:

```bash
make test        # 14 assertions against a stub libei, no session needed
```

And you can see the arithmetic end to end:

```bash
python3 tools/simulate.py
```

```
现状（未装垫片）: EI单位=[-120, -2040, -1920, ...]   mac行数=[3, 51, 48, 51, ...]
装垫片后:        EI单位=[-120, -120, -120, ...]      mac行数=[3, 3, 3, 3, ...]
```

## 8. I need to disable it right now

```bash
systemctl --user stop synergy-3.service
EISFIX_DISABLE=1 LD_PRELOAD=... # or just:
./scripts/uninstall.sh
```

`EISFIX_DISABLE=1` in the drop-in makes every interposed function call straight
through to the real implementation, which is useful for A/B testing without
uninstalling.

## 9. Collecting a bug report

```bash
{
  echo "== versions =="
  /opt/Synergy/synergy-core --version
  hyprctl version 2>/dev/null | head -3
  echo "== session =="
  echo "XDG_SESSION_TYPE=$XDG_SESSION_TYPE"
  echo "== shim env =="
  pid=$(pgrep -x synergy-core | head -1)
  tr '\0' '\n' < /proc/$pid/environ | grep -E 'LD_PRELOAD|EISFIX'
  echo "== mappings =="
  grep ei_scroll_fix /proc/$pid/maps
  echo "== last scrolls =="
  tail -20 ~/.local/state/Synergy/scroll-fix.log
  echo "== device caps (REL_WHEEL_HI_RES = bit 11 = 0x800) =="
  for f in /sys/class/input/event*/device/capabilities/rel; do
    echo "$(cat ${f%/*}/../name 2>/dev/null): $(cat $f)"
  done
} > /tmp/ei-scroll-fix-report.txt
```

Redact the hostname/IP lines before posting publicly.
