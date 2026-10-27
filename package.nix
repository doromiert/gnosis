{ pkgs, brave ? pkgs.brave }:
let
  lib = pkgs.lib;
  system = "x86_64-linux";
  jupiterGtk = pkgs.gtk4.overrideAttrs (old: {
    patches = (old.patches or []) ++ [
      ./patches/gtk4-x11-ignore-stale-selection-clear.patch
      ./patches/gtk4-wayland-follow-fastest-intersecting-output.patch
    ];
  });
  uiLibraries = with pkgs; [ gtk4 libadwaita json-glib libepoxy fontconfig freetype libsodium libsecret libsoup_3 qrencode ];
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
      # Chromium's bundled loader does not search NixOS's graphics-driver path.
      rm "$out/Release/libvulkan.so.1"
      ln -s ${pkgs.vulkan-loader}/lib/libvulkan.so.1 "$out/Release/libvulkan.so.1"
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
  keepassxcBrowser = pkgs.stdenvNoCC.mkDerivation {
    pname = "jupiter-keepassxc-browser";
    version = "1.10.4.1";
    src = pkgs.fetchurl {
      url = "https://github.com/keepassxreboot/keepassxc-browser/releases/download/1.10.4.1/keepassxc-browser_1.10.4.1_chromium.zip";
      hash = "sha256-PM2mEqlR3UeItCTNQa3O2DKSLU5ILqPuA8YBM5D0WEI=";
    };
    nativeBuildInputs = [ pkgs.unzip pkgs.python3 ];
    unpackPhase = "unzip -q $src";
    patchPhase = "python3 ${./engine/tools/prepare-keepassxc.py} . ${./engine/data/keepassxc}";
    dontConfigure = true;
    dontBuild = true;
    installPhase = "mkdir -p $out; cp -r . $out/";
    meta.license = lib.licenses.gpl3;
  };
  package = pkgs.epiphany.overrideAttrs (old: {
    pname = "gnosis";
    version = "0.1.0";
    src = lib.cleanSourceWith {
      src = lib.cleanSource ./.;
      filter = path: type: !(builtins.elem (baseNameOf path) [ "JUPITER-TODO.md" "BRAVE-MIGRATION.md" ]);
    };
    patches = [];
    nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.cmake pkgs.wayland-scanner ];
    buildInputs = map (input:
      if input == pkgs.gtk4.dev then jupiterGtk.dev
      else if input == pkgs.gtk4 then jupiterGtk
      else input
    ) old.buildInputs ++ [ pkgs.libpwquality pkgs.libepoxy pkgs.wlroots_0_19 pkgs.wayland pkgs.wayland-protocols pkgs.libxkbcommon pkgs.libdrm pkgs.libglvnd pkgs.pixman pkgs.qrencode ] ++ chromiumLibraries;
    preConfigure = (old.preConfigure or "") + ''
      export PKG_CONFIG_PATH="${jupiterGtk.dev}/lib/pkgconfig:''${PKG_CONFIG_PATH-}"
      test "$(pkg-config --variable=libdir gtk4)" = "${lib.getLib jupiterGtk}/lib"
      export NIX_LDFLAGS="-rpath ${lib.getLib jupiterGtk}/lib ''${NIX_LDFLAGS-}"
    '';
    mesonFlags = [ "-Dgranite=disabled" "-Dunit_tests=disabled" "-Dman-pages=disabled" "-Dcef_root=${cefSdk}" "-Dcef_runtime=linked" "-Dublock_dir=${ublock}" "-Dkeepassxc_dir=${keepassxcBrowser}" ];
    preFixup = (old.preFixup or "") + ''
      gappsWrapperArgs+=(--set JUPITER_ENGINE brave)
      gappsWrapperArgs+=(--set-default JUPITER_SURFACE wayland)
      gappsWrapperArgs+=(--set-default JUPITER_BRAVE_EXECUTABLE ${brave}/bin/brave)
      gappsWrapperArgs+=(--set-default JUPITER_BRAVE_VERSION ${brave.version})
      gappsWrapperArgs+=(--set JUPITER_NOTIFICATION_ICON "$out/share/icons/hicolor/scalable/apps/io.github.doromiert.Jupiter.svg")
      ${lib.optionalString (brave ? nativeInstallShim) "gappsWrapperArgs+=(--set-default JUPITER_NATIVE_UI_BRIDGE 1)"}
    '';
    postFixup = (old.postFixup or "") + ''
      # Retain the old command as an alias for existing launch scripts.
      ln -s gnosis "$out/bin/jupiter"
    '';
    passthru = (old.passthru or {}) // { inherit cefSdk ublock jupiterGtk keepassxcBrowser; };
    meta = old.meta // {
      description = "GNOME Web shell with a composited Brave web-content backend";
      homepage = "https://github.com/doromiert/gnosis";
      mainProgram = "gnosis";
    };
  });
in package
