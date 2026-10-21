#!/bin/zsh
# Builds the Windows version (cross-compiled, see mingw-x64.cmake) and assembles the folder to copy to the PC:
#   Les Simpson - Le Jeu.exe, dxcompiler.dll, dxil.dll, game/ (French disc data), modifications/, LISEZMOI.txt
# Game data comes from the Mac application (APFS clones: no extra disk space).
set -e
here="${0:A:h}"
app="/Applications/Jeux/Les Simpson - Le Jeu.app"
out="$here/Les Simpson - Le Jeu (Windows)"
dxc="$here/../XenosRecomp/thirdparty/dxc-bin/bin/x64"

cmake --build "$here/build" > /dev/null
mkdir -p "$out"
cp "$here/build/SimpsonsRecomp.exe" "$out/Les Simpson - Le Jeu.exe"
cp "$dxc/dxcompiler.dll" "$dxc/dxil.dll" "$out/"
[[ -d "$out/game" ]] || cp -cR "$app/Contents/Resources/game" "$out/game"
rm -rf "$out/modifications"
cp -cR "$app/Contents/Resources/modifications" "$out/modifications"
cp "$here/LISEZMOI.txt" "$out/LISEZMOI.txt"
echo "Dossier pret : $out"
