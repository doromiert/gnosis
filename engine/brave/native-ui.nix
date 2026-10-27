{ pkgs, brave ? pkgs.brave }:
let
  pin = builtins.fromJSON (builtins.readFile ./patches/binary-native-ui.json);
  installPin = builtins.fromJSON (builtins.readFile ./patches/native-install-ui.json);
  config = pkgs.writeText "jupiter-native-install-config.h" (pkgs.lib.concatStringsSep "\n"
    (pkgs.lib.mapAttrsToList (key: value:
      "#define UI_${pkgs.lib.toUpper key} " +
      (if pkgs.lib.hasSuffix "_rva" key then "${value}UL" else builtins.toJSON value)
    ) (builtins.removeAttrs installPin [ "schema" ])));
  debugPatch = id: name:
    let
      patch = pkgs.lib.findFirst (item: item.function == name) (throw "Missing pinned native UI patch: ${name}") pin.patches;
      length = builtins.div (builtins.stringLength patch.replacement) 2;
      bytes = pkgs.lib.concatMapStringsSep "," (index: "0x" + builtins.substring (index * 2) 2 patch.expected_prefix) (pkgs.lib.range 0 (length - 1));
    in "{\"${id}\",${toString patch.virtual_address}UL,{${bytes}},${toString length}}";
  debugConfig = pkgs.writeText "jupiter-native-debug-config.h" (''
    struct DebugOptionalPatch { const char *id; uintptr_t offset; unsigned char original[3]; size_t length; };
    static const struct DebugOptionalPatch debug_optional_patches[] = {
      ${debugPatch "zoom-osd" "ZoomBubbleCoordinator::Show"},
      ${debugPatch "script-dialogs" "BrowserWebContentsDelegate::GetJavaScriptDialogManager"},
      ${debugPatch "infobars" "InfoBarContainerView::CalculatePreferredSize"}
    };
  '');
  nativeInstallShim = pkgs.stdenv.mkDerivation {
    pname = "jupiter-native-install-ui";
    version = brave.version;
    dontUnpack = true;
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [ pkgs.glib pkgs.json-glib pkgs.dbus ];
    buildPhase = ''
      cat ${config} > jupiter-native-install-config.h
      echo >> jupiter-native-install-config.h
      cat ${debugConfig} >> jupiter-native-install-config.h
      cp ${./native-authenticator-ui.inc} native-authenticator-ui.inc
      cp ${./native-find.inc} native-find.inc
      cp ${./native-close.inc} native-close.inc
      $CC -shared -fPIC -O2 -Wall -Wextra ${./native-install-ui.c} ${./native-notifications.c} -I. \
        $(pkg-config --cflags --libs glib-2.0 json-glib-1.0 dbus-1) -ldl -o native-install-ui.so
    '';
    installPhase = "install -Dm755 native-install-ui.so $out/lib/native-install-ui.so";
  };
in
assert pkgs.lib.assertMsg (brave.version == pin.brave_version)
  "Jupiter native UI binary patch: unsupported Brave version; review the binary pin before upgrading.";
pkgs.runCommand "jupiter-brave-native-ui-${brave.version}" {
  nativeBuildInputs = [ pkgs.python3 ];
  inherit (brave) version meta;
  passthru = { inherit nativeInstallShim; };
} ''
  python3 ${../tools/patch-brave-binary.py} ${brave} "$out" \
    --manifest ${./patches/binary-native-ui.json} \
    --native-install-shim ${nativeInstallShim}/lib/native-install-ui.so
''
