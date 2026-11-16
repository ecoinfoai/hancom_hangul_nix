{
  description = "Hancom Office Hwp packaged natively for Nix (no Docker)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        config.allowUnfreePredicate =
          pkg:
          builtins.elem (nixpkgs.lib.getName pkg) [
            "hancom-hwp"
            "hancom-fonts"
          ];
      };
    in
    {
      packages.${system} = {
        hancom-hwp = pkgs.callPackage ./package.nix { }; # fcitx5 (default)
        hancom-hwp-ibus = pkgs.callPackage ./package.nix { inputMethod = "ibus"; };
        # bundled Korean fonts for other apps; override fontSets for more (see fonts.nix)
        hancom-fonts = pkgs.callPackage ./fonts.nix { inherit (self.packages.${system}) hancom-hwp; };
        default = self.packages.${system}.hancom-hwp;
      };

      apps.${system}.default = {
        type = "app";
        program = nixpkgs.lib.getExe self.packages.${system}.default;
      };

      overlays.default = final: prev: {
        hancom-hwp = final.callPackage ./package.nix { };
        hancom-fonts = final.callPackage ./fonts.nix { };
      };

      # Heavy: each check unpacks the full Hwp install and runs it under Xvfb.
      checks.${system} =
        let
          smoke = pkgs.callPackage ./tests/smoke.nix { };
          paste = pkgs.callPackage ./tests/paste.nix { };
          cursorScale = pkgs.callPackage ./tests/cursor-scale.nix { };
          bindNow = pkgs.callPackage ./tests/bind-now.nix { };
          hidpiTitleBar = pkgs.callPackage ./tests/hidpi-title-bar.nix { };
        in
        {
          hancom-hwp-cursor-scale = cursorScale {
            base = self.packages.${system}.hancom-hwp;
            scaled = self.packages.${system}.hancom-hwp.override { cursorScale = 4; };
            factor = 4;
          };
          hancom-hwp-pastes-from-clipboard = paste (
            self.packages.${system}.hancom-hwp.override { pasteFromClipboard = true; }
          );
          hancom-hwp-ibus-pastes-from-clipboard = paste (
            self.packages.${system}.hancom-hwp-ibus.override { pasteFromClipboard = true; }
          );
          hancom-hwp-starts = smoke { hwp = self.packages.${system}.hancom-hwp; };
          hancom-hwp-ibus-starts = smoke {
            hwp = self.packages.${system}.hancom-hwp-ibus;
            # Qt dlopens it for themed, sized cursors.
            loads = [ "libXcursor.so.1.0.2" ];
          };
          hancom-hwp-ibus-system-qt-starts = smoke {
            hwp = self.packages.${system}.hancom-hwp.override {
              inputMethod = "ibus";
              bundledQt = false;
            };
            loads = [
              "libQt5Core.so.${pkgs.libsForQt5.qtbase.version}"
              "libibusplatforminputcontextplugin.so"
            ];
          };
          hancom-hwp-binds-all-symbols = bindNow self.packages.${system}.hancom-hwp;
          hancom-hwp-ibus-binds-all-symbols = bindNow self.packages.${system}.hancom-hwp-ibus;
          hancom-hwp-ibus-system-qt-binds-all-symbols = bindNow (
            self.packages.${system}.hancom-hwp.override {
              inputMethod = "ibus";
              bundledQt = false;
              pasteFromClipboard = true;
            }
          );
          hancom-hwp-hidpi-title-bar = hidpiTitleBar self.packages.${system}.hancom-hwp;
          hancom-hwp-ibus-hidpi-title-bar = hidpiTitleBar self.packages.${system}.hancom-hwp-ibus;
          hancom-hwp-ibus-system-qt-hidpi-title-bar = hidpiTitleBar (
            self.packages.${system}.hancom-hwp.override {
              inputMethod = "ibus";
              bundledQt = false;
              pasteFromClipboard = true;
            }
          );
        };

      formatter.${system} = pkgs.nixfmt;
    };
}
