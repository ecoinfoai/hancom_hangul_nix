{
  description = "Hancom Office Hwp 2020 packaged natively for Nix (no Docker)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        config.allowUnfreePredicate = pkg: nixpkgs.lib.getName pkg == "hancom-hwp";
      };
    in
    {
      packages.${system} = {
        hancom-hwp = pkgs.callPackage ./package.nix { }; # fcitx5 (default)
        hancom-hwp-ibus = pkgs.callPackage ./package.nix { inputMethod = "ibus"; };
        default = self.packages.${system}.hancom-hwp;
      };

      apps.${system}.default = {
        type = "app";
        program = nixpkgs.lib.getExe self.packages.${system}.default;
      };

      overlays.default = final: prev: {
        hancom-hwp = final.callPackage ./package.nix { };
      };

      formatter.${system} = pkgs.nixfmt;
    };
}
