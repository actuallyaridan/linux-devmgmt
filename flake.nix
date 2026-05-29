{
  description = "Linux Device Manager – a Qt6 recreation of the Windows Device Manager";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
  let
    supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
    forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
  in {
    packages = forAllSystems (system:
      let pkgs = nixpkgs.legacyPackages.${system}; in {
        default = pkgs.stdenv.mkDerivation {
          pname   = "linux-devmgmt";
          version = "1.1";
          src     = ./.;

          nativeBuildInputs = with pkgs; [ cmake qt6.wrapQtAppsHook ];
          buildInputs       = with pkgs; [ qt6.qtbase ];

          cmakeFlags = [ "-DCMAKE_INSTALL_PREFIX=${placeholder "out"}" ];

          qtWrapperArgs = [
            "--prefix" "PATH" ":" (pkgs.lib.makeBinPath [
              pkgs.kmod
              pkgs.pciutils
              pkgs.bluez
            ])
            "--set" "HWDATA_DIR" "${pkgs.hwdata}/share/hwdata"
            "--set" "LIBDRM_DIR" "${pkgs.libdrm}/share/libdrm"
          ];

          meta = with pkgs.lib; {
            description = "Windows Device Manager clone for Linux";
            license     = licenses.gpl3Only;
            platforms   = [ "x86_64-linux" "aarch64-linux" ];
            mainProgram = "devmgmt";
          };
        };
      }
    );

    devShells = forAllSystems (system:
      let pkgs = nixpkgs.legacyPackages.${system}; in {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.default ];
          packages   = with pkgs; [ gdb qtcreator ];
        };
      }
    );
  };
}
