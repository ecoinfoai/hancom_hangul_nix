# SPDX-License-Identifier: MIT
#
# LD_PRELOAD library that makes Hwp paste from CLIPBOARD and keeps it from
# hanging on selection owners that never answer (see xcb_shim.c).
{
  lib,
  stdenv,
  libxcb,
  xvfb-run,
}:

stdenv.mkDerivation {
  name = "hwp-xcb-shim";
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./xcb_shim.c
      ./xcb_shim_test.c
    ];
  };

  nativeBuildInputs = [ xvfb-run ];
  buildInputs = [ libxcb ];

  buildPhase = ''
    runHook preBuild
    $CC -shared -fPIC -fno-stack-protector -o xcb_shim.so xcb_shim.c -ldl -lxcb
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    $CC -o xcb_shim_test xcb_shim_test.c -lxcb -lxcb-xfixes
    LD_PRELOAD=$PWD/xcb_shim.so xvfb-run -a ./xcb_shim_test
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 xcb_shim.so $out/lib/xcb_shim.so
    runHook postInstall
  '';
}
