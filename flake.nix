{
  description = "esp-wifi-loader build dependencies (ESP-IDF host tools)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          # Host tools only. ESP-IDF itself and its cross toolchains are
          # fetched by scripts/esp-deps.sh into target/ (same split as DMesh).
          deps = pkgs.symlinkJoin {
            name = "esp-wifi-loader-deps";
            paths = [
              pkgs.bashInteractive
              pkgs.cmake
              pkgs.coreutils
              pkgs.curl
              pkgs.dfu-util
              pkgs.findutils
              pkgs.gawk
              pkgs.git
              pkgs.gnugrep
              pkgs.gnumake
              pkgs.gnused
              pkgs.libusb1
              pkgs.ninja
              pkgs.openssl # tools/sign.py fallback and tests
              pkgs.pkg-config
              pkgs.python3
              pkgs.stdenv.cc
              pkgs.unzip
              pkgs.which
              pkgs.xz
            ];
            meta.priority = 1;
          };
          default = self.packages.${system}.deps;
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            packages = [ self.packages.${system}.deps ];
            shellHook = ''
              [ -f ./env.sh ] && . ./env.sh
            '';
          };
        }
      );
    };
}
