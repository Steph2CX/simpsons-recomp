#!/bin/zsh
# Construit (ou met a jour) l'application complete « Les Simpson - Le Jeu » dans /Applications/Jeux :
#   Contents/MacOS/SimpsonsRecomp      programme recompile autonome (simpsons/build-pgo, sinon build-portable : SDL3, FFmpeg et
#                                      MoltenVK integres, macOS 15 minimum, puce Apple)
#   Contents/Frameworks/               bibliotheque DXC (traduction des shaders)
#   Contents/Resources/game/           donnees du disque francais
#   Contents/Resources/modifications/  textes adaptes au Mac (simpsons/modifications/textes_fr.tsv)
#   Contents/Resources/icone.icns      icone (donut)
# Les sauvegardes sont dans ~/Library/Application Support/Les Simpson - Le Jeu/donnees.
# La premiere fois, les donnees du disque (simpsons/game) et les sauvegardes (simpsons/userdata) sont
# deplacees dans l'application et a leur place, et remplacees dans le projet par des liens.
set -e

project="${0:A:h}/../simpsons"
app="/Applications/Jeux/Les Simpson - Le Jeu.app"
support="$HOME/Library/Application Support/Les Simpson - Le Jeu"

mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources" "$support"

# Donnees du disque : deplacees une fois, le projet garde un lien
if [[ ! -L "$project/game" && -d "$project/game" ]]; then
    rm -rf "$app/Contents/Resources/game"
    mv "$project/game" "$app/Contents/Resources/game"
    ln -s "$app/Contents/Resources/game" "$project/game"
fi

# Sauvegardes : deplacees une fois a l'emplacement standard, le projet garde un lien
if [[ ! -L "$project/userdata" && -d "$project/userdata" ]]; then
    rm -rf "$support/donnees"
    mv "$project/userdata" "$support/donnees"
    ln -s "$support/donnees" "$project/userdata"
fi
mkdir -p "$support/donnees" "$support/recomp"

# Textes modifies, livres avec l'application (lus avant ceux du disque)
rm -rf "$app/Contents/Resources/modifications"
for f in $(cd -P "$app/Contents/Resources/game" && find . -name "cae47a5a.str"); do
    python3 -I "${0:A:h}/modifier_textes.py" "$app/Contents/Resources/game/$f" \
        "$app/Contents/Resources/modifications/$f" "$project/modifications/textes_fr.tsv" 2> /dev/null
done
# Ecran des mentions legales Fox retire du demarrage (tools/retirer_fox.py)
python3 -I "${0:A:h}/retirer_fox.py" "$app/Contents/Resources/game/frontend/frontend.str" \
    "$app/Contents/Resources/modifications/frontend/frontend.str"
# Images remplacees ("EA | HD" -> "EA | 4K ULTRA HD" : tools/logo_4k.py)
cp -R "$project/modifications/textures" "$app/Contents/Resources/modifications/textures"

# Programme et bibliotheque DXC, cherchee a cote du programme
# Optimized with the execution profile (build-pgo: -fprofile-instr-use=portable/pgo/simpsons.profdata, see
# tools/entrainer_pgo.sh); falls back on build-portable
build="$project/build-pgo"
[[ -d "$build" ]] || build="$project/build-portable"
cmake --build "$build" > /dev/null
# Le programme ne doit dependre que du systeme et de DXC (sinon il ne demarre pas sur un Mac sans Homebrew)
externes=$(otool -L "$build/SimpsonsRecomp" | tail -n +2 | awk '{print $1}' | grep -v -e '^/System/' -e '^/usr/lib/' -e '^@rpath/libdxcompiler.dylib$' || true)
if [[ -n "$externes" ]]; then
    echo "Erreur : le programme depend de bibliotheques absentes des autres Mac :\n$externes" >&2
    exit 1
fi
cp "$build/SimpsonsRecomp" "$app/Contents/MacOS/SimpsonsRecomp"
cp "$project/../XenosRecomp/thirdparty/dxc-bin/lib/arm64/libdxcompiler.dylib" "$app/Contents/Frameworks/"
codesign --force -s - "$app/Contents/Frameworks/libdxcompiler.dylib" "$app/Contents/MacOS/SimpsonsRecomp"

cat > "$app/Contents/MacOS/lancer" <<'LANCEUR'
#!/bin/zsh
# Lance Les Simpson - Le Jeu (recompilation Xbox 360, disque francais)
app="${0:A:h:h}"
support="$HOME/Library/Application Support/Les Simpson - Le Jeu"
mkdir -p "$support/donnees" "$support/recomp"
cd "$support"
# Reglages (variables SIMPSONS_..., voir le README) : une par ligne dans reglages.txt, # pour un commentaire
if [[ -f "$support/reglages.txt" ]]; then
    while IFS= read -r ligne; do
        [[ "$ligne" =~ '^SIMPSONS_[A-Z0-9_]+=[^[:space:]]*$' ]] && export "$ligne"
    done < "$support/reglages.txt"
fi
SIMPSONS_DONNEES="$support/donnees" exec "$app/MacOS/SimpsonsRecomp" "$app/Resources/game/default.xex" > "$support/jeu.log" 2>&1
LANCEUR
chmod +x "$app/Contents/MacOS/lancer"

cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>Les Simpson - Le Jeu</string>
	<key>CFBundleDisplayName</key><string>Les Simpson - Le Jeu</string>
	<key>CFBundleIdentifier</key><string>local.simpsons.recomp</string>
	<key>CFBundleExecutable</key><string>lancer</string>
	<key>CFBundleIconFile</key><string>icone</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleVersion</key><string>1.0</string>
	<key>LSMinimumSystemVersion</key><string>15.0</string>
	<key>LSArchitecturePriority</key><array><string>arm64</string></array>
	<key>LSApplicationCategoryType</key><string>public.app-category.games</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
# Signature ad hoc de l'application entiere (copie vers un autre Mac)
codesign --force --deep -s - "$app"
touch "$app"
echo "Application prete : $app"
