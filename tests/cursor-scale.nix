# SPDX-License-Identifier: MIT
#
# Checks that every Hwp cursor in `scaled` is `factor` times the size of the
# same cursor in `base`, and that the wrapper drops XCURSOR_SIZE.
{
  runCommand,
  python3,
}:

{
  base,
  scaled,
  factor,
}:

runCommand "${scaled.name}-cursor-scale" { nativeBuildInputs = [ python3 ]; } ''
  grep -q '^unset XCURSOR_SIZE$' ${scaled}/bin/hwp \
    || { echo "the wrapper keeps XCURSOR_SIZE" >&2; exit 1; }
  python3 -I - ${base} ${scaled} ${toString factor} <<'PY'
  import pathlib, sys

  base, scaled, factor = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3])
  cursors = sorted(base.rglob("*.cur"))
  assert cursors, "no cursors found"
  for cur in cursors:
      size = lambda p: tuple(b or 256 for b in p.read_bytes()[6:8])
      want = tuple(n * factor for n in size(cur))
      got = size(scaled / cur.relative_to(base))
      assert got == want, f"{cur.name}: {got}, expected {want}"
  print(f"{len(cursors)} cursors scaled by {factor}")
  PY
  touch $out
''
