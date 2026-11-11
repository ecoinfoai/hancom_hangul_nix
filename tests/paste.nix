# SPDX-License-Identifier: MIT
#
# Puts different text in CLIPBOARD and PRIMARY, presses Ctrl+V in a new Hwp
# document, then copies the document back out and expects the CLIPBOARD text.
{
  lib,
  runCommand,
  dbus,
  xclip,
  xdotool,
  xvfb-run,
}:

hwp:

runCommand "${hwp.name}-paste"
  {
    nativeBuildInputs = [
      dbus
      xclip
      xdotool
      xvfb-run
    ];
  }
  ''
    export HOME=$(mktemp -d)
    export XDG_CURRENT_DESKTOP=GNOME
    # See smoke.nix.
    export DBUS_FATAL_WARNINGS=0

    xvfb-run -a dbus-run-session --config-file=${dbus}/share/dbus-1/session.conf -- sh -c '
      printf CLIPTEXT | xclip -selection clipboard -i
      printf PRIMTEXT | xclip -selection primary -i
      ${lib.getExe hwp} > "$HOME/hwp.log" 2>&1 &
      pid=$!
      for _ in $(seq 120); do
        w=$(xdotool search --onlyvisible --name "한글" | head -n 1)
        [ -n "$w" ] && break
        sleep 1
      done
      if [ -z "$w" ]; then
        echo "hwp did not map a window within 120 s" >&2
        cat "$HOME/hwp.log" >&2
        exit 1
      fi
      sleep 5
      xdotool windowfocus --sync $w
      xdotool key --clearmodifiers ctrl+v
      sleep 2
      xdotool key --clearmodifiers ctrl+a
      sleep 1
      xdotool key --clearmodifiers ctrl+c
      sleep 2
      pasted=$(timeout 5 xclip -o -selection clipboard)
      kill $pid
      if [ "$pasted" != CLIPTEXT ]; then
        echo "Ctrl+V pasted \"$pasted\", expected CLIPTEXT" >&2
        exit 1
      fi
    '
    touch $out
  ''
