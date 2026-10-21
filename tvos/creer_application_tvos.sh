#!/bin/zsh
# Construit l'application Apple TV « Les Simpson » dans tvos/Les Simpson.app et l'installe sur l'Apple TV :
#   SimpsonsRecomp            programme recompile pour tvOS (simpsons/build-tvos : SDL3, FFmpeg et MoltenVK integres)
#   Frameworks/               bibliotheque DXC compilee pour tvOS (tvos/build-dxc-tvos)
#   game/                     donnees du disque francais (clone APFS de celles de l'application Mac : pas de place en plus)
#   modifications/            textes et images modifies (repris de l'application Mac)
#   Assets.car                icone (donut sur le ciel de l'ecran titre) et images Top Shelf, compilees depuis
#                             tvos/icone/Assets.xcassets (tvos/icone/fabriquer_images.swift)
# Reglages personnels (equipe, identifiant de l'application, Apple TV, certificat) : tvos/config.local (voir
# tvos/config.exemple). Signature avec le profil Xcode de l'identifiant, cree au premier passage (tvos/profil).
# Sauvegardes et journal sur l'Apple TV : Library/Caches/Les Simpson - Le Jeu (jeu.log).
# Options : sans_installation (ne fait que construire), console (lance le jeu et affiche son journal ici).
set -e

racine="${0:A:h:h}"
build="$racine/simpsons/build-tvos"
mac="/Applications/Jeux/Les Simpson - Le Jeu.app/Contents/Resources"
app="$racine/tvos/Les Simpson.app"
[[ -f "$racine/tvos/config.local" ]] || { echo "Erreur : copier tvos/config.exemple en tvos/config.local et le remplir" >&2; exit 1; }
source "$racine/tvos/config.local"

cmake --build "$build" > /dev/null
# Le programme ne doit dependre que du systeme et de DXC
externes=$(otool -L "$build/SimpsonsRecomp" | tail -n +2 | awk '{print $1}' | grep -v -e '^/System/' -e '^/usr/lib/' -e '^@rpath/libdxcompiler.dylib$' || true)
if [[ -n "$externes" ]]; then
    echo "Erreur : le programme depend de bibliotheques absentes de l'Apple TV :\n$externes" >&2
    exit 1
fi

mkdir -p "$app/Frameworks"
cp "$build/SimpsonsRecomp" "$app/SimpsonsRecomp"
cp "$racine/tvos/build-dxc-tvos/lib/libdxcompiler.dylib" "$app/Frameworks/"
[[ -d "$app/game" ]] || cp -cR "$mac/game" "$app/game"
rm -rf "$app/modifications"
cp -cR "$mac/modifications" "$app/modifications"
# Icone : images faites depuis une capture de l'ecran titre (pas dans le depot, voir tvos/icone/fabriquer_images.swift)
if [[ -d "$racine/tvos/icone/Assets.xcassets" ]]; then
    xcrun actool "$racine/tvos/icone/Assets.xcassets" --compile "$app" --platform appletvos --minimum-deployment-target 17.0 \
        --target-device tv --app-icon "App Icon & Top Shelf Image" --output-partial-info-plist /dev/null > /dev/null
else
    echo "Pas d'icone : tvos/icone/Assets.xcassets absent (voir tvos/icone/fabriquer_images.swift)"
fi

cat > "$app/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>Les Simpson - Le Jeu</string>
	<key>CFBundleDisplayName</key><string>Les Simpson - Le Jeu</string>
	<key>CFBundleIdentifier</key><string>IDENTIFIANT</string>
	<key>CFBundleExecutable</key><string>SimpsonsRecomp</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleSupportedPlatforms</key><array><string>AppleTVOS</string></array>
	<key>DTPlatformName</key><string>appletvos</string>
	<key>MinimumOSVersion</key><string>17.0</string>
	<key>UIDeviceFamily</key><array><integer>3</integer></array>
	<key>UIRequiredDeviceCapabilities</key><array><string>arm64</string></array>
	<key>CFBundleIcons</key><dict><key>CFBundlePrimaryIcon</key><string>App Icon</string></dict>
	<key>TVTopShelfImage</key>
	<dict>
		<key>TVTopShelfPrimaryImage</key><string>Top Shelf Image</string>
		<key>TVTopShelfPrimaryImageWide</key><string>Top Shelf Image Wide</string>
	</dict>
	<key>GCSupportsControllerUserInteraction</key><true/>
	<key>GCSupportedGameControllers</key>
	<array>
		<dict><key>ProfileName</key><string>ExtendedGamepad</string></dict>
		<dict><key>ProfileName</key><string>MicroGamepad</string></dict>
	</array>
</dict>
</plist>
PLIST

sed -i '' "s|<string>IDENTIFIANT</string>|<string>$IDENTIFIANT</string>|" "$app/Info.plist"

# Profil de l'application : Xcode le cree (et le renouvelle) en construisant un petit projet du meme identifiant
profils="$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"
profil=$(grep -l "$EQUIPE.$IDENTIFIANT<" "$profils/"*.mobileprovision 2>/dev/null | head -1 || true)
if [[ -z "$profil" ]]; then
    (cd "$racine/tvos/profil" && EQUIPE="$EQUIPE" IDENTIFIANT="$IDENTIFIANT" xcodegen generate > /dev/null &&
        xcodebuild -project Simpsons.xcodeproj -scheme Simpsons -destination 'generic/platform=tvOS' \
            -allowProvisioningUpdates -derivedDataPath build build > /dev/null)
    profil=$(grep -l "$EQUIPE.$IDENTIFIANT<" "$profils/"*.mobileprovision | head -1)
fi
cp "$profil" "$app/embedded.mobileprovision"
droits="${TMPDIR:-/tmp}/simpsons-droits.plist"
security cms -D -i "$profil" | plutil -extract Entitlements xml1 - -o "$droits"
codesign --force -s "$IDENTITE" "$app/Frameworks/libdxcompiler.dylib"
codesign --force -s "$IDENTITE" --entitlements "$droits" "$app"
echo "Application prete : $app"

[[ "$1" == sans_installation ]] && exit 0
xcrun devicectl device install app --device "$APPAREIL" "$app" > /dev/null
echo "Installee sur l'Apple TV"
if [[ "$1" == console ]]; then
    xcrun devicectl device process launch --device "$APPAREIL" --console --terminate-existing \
        --environment-variables '{"SIMPSONS_CONSOLE": "1"}' "$IDENTIFIANT"
fi
