{ pkgs }:
let
  lib = pkgs.lib;
  system = "x86_64-linux";
  uiLibraries = with pkgs; [ gtk4 libadwaita json-glib libepoxy fontconfig freetype libsodium libsecret libsoup_3 ];
  chromiumLibraries = with pkgs; [
    stdenv.cc.cc.lib glib nss nspr dbus at-spi2-core systemd
    cups libdrm libxkbcommon libGL libgbm expat alsa-lib pango cairo
    libX11 libXcomposite libXdamage libXext libXfixes libXrandr libxcb
  ];
  buildTools = with pkgs; [ cmake ninja pkg-config sysprof nodejs ];
  cefSdk = pkgs.stdenv.mkDerivation {
    pname = "jupiter-cef-sdk";
    version = "150.0.22";
    src = pkgs.fetchurl {
      url = "https://cef-builds.spotifycdn.com/cef_binary_150.0.22+gc1a097e+chromium-150.0.7871.256_linux64_minimal.tar.bz2";
      hash = "sha256-3Rtcw6xu/mWex+jpsGd7pOQyFqL+/K/Vq8k0CDfjLZY=";
    };
    nativeBuildInputs = [ pkgs.autoPatchelfHook ];
    buildInputs = chromiumLibraries;
    dontConfigure = true;
    dontBuild = true;
    installPhase = ''
      runHook preInstall
      mkdir -p "$out"
      cp -r . "$out/"
      runHook postInstall
    '';
    meta = {
      description = "Pinned stable Chromium Embedded Framework SDK";
      homepage = "https://github.com/chromiumembedded/cef";
      license = lib.licenses.bsd3;
      platforms = [ system ];
    };
  };
  ublock = pkgs.stdenvNoCC.mkDerivation {
    pname = "ublock-origin";
    version = "1.75.0";
    src = pkgs.fetchurl {
      url = "https://github.com/gorhill/uBlock/releases/download/1.75.0/uBlock0_1.75.0.chromium.zip";
      hash = "sha256-OTz5VwnRB01AIpcOkBTkNDlcU6giOH8eJfQ76Xz0tYI=";
    };
    nativeBuildInputs = [ pkgs.unzip ];
    dontConfigure = true;
    dontBuild = true;
    installPhase = "mkdir -p $out; cp -r . $out/";
    meta.license = lib.licenses.gpl3Plus;
  };
  package = pkgs.epiphany.overrideAttrs (old: {
    pname = "jupiter";
    version = "0.1.0";
    src = lib.cleanSource ./.;
    patches = [];
    nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.cmake ];
    buildInputs = old.buildInputs ++ [ pkgs.libpwquality pkgs.libepoxy ] ++ chromiumLibraries;
    mesonFlags = [ "-Dgranite=disabled" "-Dunit_tests=enabled" "-Dman-pages=disabled" "-Dcef_root=${cefSdk}" "-Dublock_dir=${ublock}" ];
    passthru = (old.passthru or {}) // { inherit cefSdk ublock; };
    meta = old.meta // { description = "GNOME Web shell being ported to Chromium Embedded Framework"; mainProgram = "jupiter"; };
  });
in package
