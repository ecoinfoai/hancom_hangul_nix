# SPDX-License-Identifier: MIT
#
# Starts hwp on a virtual X server with a GNOME session environment and
# passes once its document window is mapped and the process is still alive.
# Every file name in `loads` must also be mapped into the running process.
{
  lib,
  runCommand,
  dbus,
  xvfb-run,
  xdotool,
}:

{
  hwp,
  loads ? [ ],
}:

runCommand "${hwp.name}-smoke"
  {
    nativeBuildInputs = [
      dbus
      xvfb-run
      xdotool
    ];
  }
  ''
    export HOME=$(mktemp -d)
    export XDG_CURRENT_DESKTOP=GNOME

    # The sandbox has no machine-id, which libdbus treats as a fatal check
    # failure unless told otherwise; a real session has one.
    export DBUS_FATAL_WARNINGS=0
    xvfb-run -a dbus-run-session --config-file=${dbus}/share/dbus-1/session.conf -- sh -c '
      ${lib.getExe hwp} > "$HOME/hwp.log" 2>&1 &
      pid=$!
      for _ in $(seq 120); do
        if ! kill -0 $pid 2> /dev/null; then
          echo "hwp exited before mapping its window" >&2
          cat "$HOME/hwp.log" >&2
          exit 1
        fi
        xdotool search --onlyvisible --name "한글" > /dev/null && break
        sleep 1
      done
      if ! xdotool search --onlyvisible --name "한글" > /dev/null; then
        echo "hwp did not map a window within 120 s" >&2
        cat "$HOME/hwp.log" >&2
        exit 1
      fi
      sleep 5
      for lib in ${lib.escapeShellArgs loads}; do
        if ! grep -q "/$lib\$" /proc/$pid/maps; then
          echo "hwp did not load $lib" >&2
          exit 1
        fi
      done
      if ! kill -0 $pid 2> /dev/null; then
        echo "hwp exited after mapping its window" >&2
        cat "$HOME/hwp.log" >&2
        exit 1
      fi
      kill $pid
    '
    touch $out
  ''
