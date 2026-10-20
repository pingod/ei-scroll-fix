#!/usr/bin/env bash
# ei-scroll-fix — install the LD_PRELOAD shim for Synergy / deskflow.
# SPDX-License-Identifier: MIT
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB_DIR="${EISFIX_LIB_DIR:-$HOME/.local/lib}"
UNIT_NAME="synergy-3.service"
UNIT_DIR="$HOME/.config/systemd/user/${UNIT_NAME}.d"
DROPIN="$UNIT_DIR/scroll-fix.conf"
DIV="${EISFIX_SMOOTH_DIV:-1.5}"
SO_SRC="$REPO_DIR/build/ei_scroll_fix.so"
SO_DST="$LIB_DIR/ei_scroll_fix.so"

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- build ------

if [ ! -f "$SO_SRC" ]; then
  say "==> building shim"
  command -v make >/dev/null || die "make not found; install build tools or copy a prebuilt .so"
  make -C "$REPO_DIR" shim
fi
[ -f "$SO_SRC" ] || die "missing $SO_SRC"

# ------------------------------------------------- self-check (interposition) -

CORE=""
for c in /opt/Synergy/synergy-core "$(command -v synergy-core || true)" \
         "$(command -v deskflow-core || true)"; do
  [ -n "$c" ] && [ -x "$c" ] && { CORE="$c"; break; }
done

if [ -n "$CORE" ]; then
  say "==> self-check: does the shim actually interpose $CORE's libei calls?"
  n=$(LD_BIND_NOW=1 LD_DEBUG=bindings LD_PRELOAD="$SO_SRC" \
        "$CORE" --version 2>&1 | grep -c 'ei_scroll_fix\.so' || true)
  if [ "${n:-0}" -ge 4 ]; then
    say "    OK — $n scroll symbols bound to the shim"
  else
    say "    WARNING — only $n symbols bound (expected >= 4); the shim may not take effect"
    say "              (if this is a static binary, LD_PRELOAD cannot work at all)"
  fi
else
  say "==> self-check skipped (no synergy-core / deskflow-core found)"
fi

# ---------------------------------------------------------------- install ----

say "==> installing to $LIB_DIR"
mkdir -p "$LIB_DIR"
install -m 0644 "$SO_SRC" "$SO_DST"

say "==> writing systemd drop-in (keeps any existing override.conf)"
mkdir -p "$UNIT_DIR"
cat > "$DROPIN" <<CONF
[Service]
Environment=LD_PRELOAD=%h/.local/lib/ei_scroll_fix.so
Environment=EISFIX_SMOOTH_DIV=${DIV}
Environment=EISFIX_DROP_DISCRETE=1
Environment=EISFIX_WHEEL_ONLY=1
Environment=EISFIX_LOG=%h/.local/state/Synergy/scroll-fix.log
CONF

if ! systemctl --user cat "$UNIT_NAME" >/dev/null 2>&1; then
  say ""
  say "    NOTE: $UNIT_NAME was not found in your user session."
  say "          The drop-in is written but nothing was restarted."
  say "          If your unit has a different name, rename $DROPIN accordingly."
  exit 0
fi

say "==> reloading and restarting $UNIT_NAME"
systemctl --user daemon-reload
systemctl --user restart "$UNIT_NAME" || die "restart failed; run: systemctl --user status $UNIT_NAME"

sleep 2

# ---------------------------------------------------------------- verify -----

say "==> verifying that the running core inherited LD_PRELOAD"
pid=$(pgrep -x synergy-core 2>/dev/null | head -1 || true)
[ -n "${pid:-}" ] || pid=$(pgrep -x deskflow-core 2>/dev/null | head -1 || true)

if [ -n "${pid:-}" ]; then
  say "    core pid=$pid"
  if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -q '^LD_PRELOAD=.*ei_scroll_fix'; then
    say "    LD_PRELOAD is set ✓"
  else
    say "    could not read environ (permissions?) — verify manually;"
    say "    a pkexec/sudo-launched core loses LD_PRELOAD, in which case use README §manual"
  fi
  if grep -q ei_scroll_fix "/proc/$pid/maps" 2>/dev/null; then
    say "    shim is mapped into the process ✓"
  fi
else
  say "    no core process found"
fi

cat <<EOF

Done.

  Verify on a real scroll (the shim is lazy — it logs on first use):
      tail -3 ~/.local/state/Synergy/scroll-fix.log

  Undo everything:
      $(dirname "${BASH_SOURCE[0]}")/uninstall.sh
EOF
