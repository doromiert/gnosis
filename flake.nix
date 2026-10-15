{
  description = "Jupiter — Epiphany with a Chromium engine";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs { inherit system; };
    package = import ./package.nix { inherit pkgs; };
  in {
    packages.${system} = { default = package; jupiter = package; cef-sdk = package.cefSdk; };
    apps.${system}.default = { type = "app"; program = "${package}/bin/jupiter"; };
    devShells.${system}.default = pkgs.mkShell {
      inputsFrom = [ package ];
      packages = with pkgs; [ cmake ninja meson pkg-config python3 xorg-server imagemagick gdb ];
      CEF_ROOT = package.cefSdk;
      LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath package.cefSdk.buildInputs;
    };
  };
}
