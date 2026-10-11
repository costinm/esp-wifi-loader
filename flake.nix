{
  description = "esp-wifi-loader signing, flashing and build dependencies";

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
          python = pkgs.python3;
          firmwareRoot = builtins.getEnv "WL_FIRMWARE_ROOT";
          tools = pkgs.runCommand "esp-wifi-loader-tools" { nativeBuildInputs = [ pkgs.makeWrapper ]; } ''
            mkdir -p $out/bin $out/lib/esp-wifi-loader $out/share/esp-wifi-loader
            cp ${./tools/sign.py} $out/lib/esp-wifi-loader/sign.py
            cp ${./tools/wl_format.py} $out/lib/esp-wifi-loader/wl_format.py
            cp ${./tools/sign-bundle.py} $out/lib/esp-wifi-loader/sign-bundle.py
            cp ${./tools/upload.py} $out/lib/esp-wifi-loader/upload.py
            makeWrapper ${python}/bin/python3 $out/bin/esp-wifi-sign --add-flags $out/lib/esp-wifi-loader/sign-bundle.py --prefix PATH : ${pkgs.openssl}/bin
            makeWrapper ${python}/bin/python3 $out/bin/esp-wifi-sign-one --add-flags $out/lib/esp-wifi-loader/sign.py --prefix PATH : ${pkgs.openssl}/bin
            makeWrapper ${python}/bin/python3 $out/bin/esp-wifi-upload --add-flags $out/lib/esp-wifi-loader/upload.py
            ln -s $out/lib/esp-wifi-loader $out/share/esp-wifi-loader/tools
          '';
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
          inherit tools;
          default = pkgs.symlinkJoin {
            name = "esp-wifi-loader";
            paths = [ tools pkgs.espflash ];
          };
          # Package already-built, unsigned images. Build them with scripts/build.sh,
          # then set WL_FIRMWARE_ROOT to its target/out directory and use --impure.
          firmware =
            if firmwareRoot == "" then throw "set WL_FIRMWARE_ROOT to target/out and pass --impure"
            else pkgs.runCommand "esp-wifi-loader-firmware" { } ''
              mkdir -p $out/share/esp-wifi-loader/flash
              cp -r ${builtins.path {
                path = /. + firmwareRoot;
                name = "esp-wifi-loader-unsigned";
                filter = path: type:
                  let
                    base = builtins.baseNameOf (toString path);
                    parent = builtins.baseNameOf (builtins.dirOf (toString path));
                    chips = [ "esp32" "esp32s3" "esp32c6" ];
                  in
                  (type == "directory" && (base == "out" || builtins.elem base chips))
                  || (type == "regular" && builtins.elem parent chips
                      && nixpkgs.lib.hasSuffix ".bin" base);
              }}/. $out/share/esp-wifi-loader/flash/
            '';
          with-firmware = pkgs.symlinkJoin {
            name = "esp-wifi-loader-with-firmware";
            paths = [ self.packages.${system}.default self.packages.${system}.firmware ];
          };
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
