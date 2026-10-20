#!/usr/bin/env bash
# ei-scroll-fix — remove the shim from the systemd unit. The .so in
# ~/.local/lib is left alone (delete it manually if you want it gone).
# SPDX-License-Identifier: MIT
set -euo pipefail

UNIT_NAME="${EISFIX_UNIT:-synergy-3.service}"
DROPIN="$HOME/.config/systemd/user/${UNIT_NAME}.d/scroll-fix.conf"
SO_DST="$HOME/.local/lib/ei_scroll_fix.so"

if [ -f "$DROPIN" ]; then
  rm -f "$DROPIN"
  echo "removed $DROPIN"
else
  echo "no drop-in at $DROPIN (already removed?)"
fi

if systemctl --user cat "$UNIT_NAME" >/dev/null 2>&1; then
  systemctl --user daemon-reload
  systemctl --user restart "$UNIT_NAME" || true
  echo "restarted $UNIT_NAME"
else
  echo "unit $UNIT_NAME not found; nothing restarted"
fi

echo
echo "The shim library is still at: $SO_DST"
echo "Remove it with:  rm -f $SO_DST"
