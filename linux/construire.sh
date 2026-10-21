#!/usr/bin/env bash
# Builds the Linux version (x64) on the Linux PC itself and assembles the game folder:
#   linux/Les Simpson - Le Jeu (Linux)/  les-simpson-le-jeu, libdxcompiler.so, game/, modifications/, lancer.sh
# Usage, from the copied xbox-recomp folder:  bash linux/construire.sh
# Game data comes from simpsons/game when it is a real folder, else from the Windows folder (copied with the rest).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
jobs="$(nproc)"

echo "== 1/5 Paquets necessaires (mot de passe administrateur demande) =="
if command -v apt-get > /dev/null; then
    sudo apt-get update
    sudo apt-get install -y clang lld cmake ninja-build make rsync pkg-config libvulkan-dev \
        vulkan-tools mesa-vulkan-drivers libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libpulse-dev libasound2-dev libpipewire-0.3-dev \
        libudev-dev libdbus-1-dev
    sudo apt-get install -y libsdl3-dev || true
elif command -v dnf > /dev/null; then
    sudo dnf install -y clang lld cmake ninja-build make rsync pkgconf-pkg-config vulkan-loader-devel \
        vulkan-tools mesa-vulkan-drivers SDL3-devel
elif command -v pacman > /dev/null; then
    sudo pacman -S --needed --noconfirm clang lld cmake ninja make rsync pkgconf vulkan-icd-loader vulkan-headers \
        vulkan-tools vulkan-intel vulkan-radeon sdl3
else
    echo "Gestionnaire de paquets inconnu : installer clang, cmake, ninja, make, Vulkan et SDL3 a la main."
fi
export CC=clang CXX=clang++

echo "== 2/5 SDL3 et FFmpeg =="
prefix="$here/prefix"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
# FFmpeg: only the XMA decoders, same version as the Mac and Windows builds (from portable/src/ffmpeg)
if [[ ! -f "$prefix/lib/libavcodec.a" ]]; then
    rm -rf "$here/src-ffmpeg"
    cp -r "$root/portable/src/ffmpeg" "$here/src-ffmpeg"
    (cd "$here/src-ffmpeg" && (make distclean > /dev/null 2>&1 || true) &&
        ./configure --prefix="$prefix" --cc=clang --cxx=clang++ --disable-everything --disable-programs --disable-doc \
            --disable-network --disable-autodetect --disable-x86asm --disable-avdevice --disable-avformat \
            --disable-swscale --disable-swresample --disable-avfilter --enable-decoder=xma1,xma2,wmapro \
            --enable-static --disable-shared --enable-pic > /dev/null &&
        make -j "$jobs" > /dev/null && make install > /dev/null)
fi
# SDL3 always built from the source shipped in portable/src/SDL and linked in (the distribution's SDL3 may be missing,
# or too recent for older systems); its X11/Wayland/PulseAudio/PipeWire/ALSA drivers are loaded at start if present
if [[ ! -f "$here/prefix-sdl/lib/libSDL3.a" ]]; then
    rm -rf "$here/build-sdl"
    cmake -S "$root/portable/src/SDL" -B "$here/build-sdl" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF \
        -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="$here/prefix-sdl"
    cmake --build "$here/build-sdl" -j "$jobs"
    cmake --install "$here/build-sdl" > /dev/null
fi

echo "== 3/5 Utilitaires XenonRecomp =="
cmake -S "$root/XenonRecomp" -B "$here/build-xenon" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=-include cstdlib"
cmake --build "$here/build-xenon" -j "$jobs" --target XenonUtils disasm fmt

echo "== 4/5 Le jeu (long : plus de 300 fichiers) =="
cmake -S "$root/simpsons/runtime" -B "$here/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_CXX_FLAGS=-march=x86-64-v3" -DCMAKE_PREFIX_PATH="$prefix" -DSDL3_DIR="$here/prefix-sdl/lib/cmake/SDL3" \
    -DXENON_RECOMP_BUILD="$here/build-xenon"
cmake --build "$here/build" -j "$jobs" --target SimpsonsRecomp

echo "== 5/5 Dossier du jeu =="
out="$here/Les Simpson - Le Jeu (Linux)"
mkdir -p "$out"
cp "$here/build/SimpsonsRecomp" "$out/les-simpson-le-jeu"
cp "$root/XenosRecomp/thirdparty/dxc-bin/lib/x64/libdxcompiler.so" "$out/"
game="$root/simpsons/game"
[[ -f "$game/default.xex" ]] || game="$root/windows/Les Simpson - Le Jeu (Windows)/game"
[[ -d "$out/game" ]] || cp -r "$game" "$out/game"
modifications="$root/windows/Les Simpson - Le Jeu (Windows)/modifications"
[[ -d "$modifications" ]] && rm -rf "$out/modifications" && cp -r "$modifications" "$out/modifications"
cat > "$out/lancer.sh" << 'EOF'
#!/usr/bin/env bash
# Lance le jeu ; journal et sauvegardes dans ~/.local/share/les-simpson-le-jeu
jeu="$(dirname "$(readlink -f "$0")")/les-simpson-le-jeu"
# Mode d'energie "performance" le temps du jeu (le portable reste sinon a 1,5 GHz : 46 au lieu de 57 images/s)
if command -v powerprofilesctl > /dev/null && powerprofilesctl list 2> /dev/null | grep -q performance; then
    exec powerprofilesctl launch -p performance -r "Les Simpson - Le Jeu" -i les-simpson-le-jeu -- "$jeu" "$@"
fi
exec "$jeu" "$@"
EOF
chmod +x "$out/lancer.sh" "$out/les-simpson-le-jeu"
# Entry in the desktop's application menu (donut icon taken from the Windows .ico)
python3 -c "from PIL import Image; im = Image.open('$root/simpsons/runtime/windows/simpsons.ico'); im.size = max(im.info['sizes']); im.save('$out/icone.png')" || true
cat > "$out/LISEZMOI.txt" << 'FIN'
LES SIMPSON - LE JEU (version Linux)
====================================

INSTALLER
- Copier tout ce dossier ou l'on veut (dossier personnel, cle USB...). Ne rien retirer du dossier :
  le programme a besoin des dossiers "game" et "modifications" et de libdxcompiler.so a cote de lui.
- Il faut : un Linux de 2022 ou plus recent (Ubuntu 22.04, Debian 12, Fedora 36...), un processeur de
  2013 ou plus recent (Intel Haswell, AMD Ryzen) et une carte graphique avec un pilote Vulkan (Intel, AMD,
  ou NVIDIA avec son pilote).

LANCER
- Double-cliquer sur "lancer.sh" (ou dans un terminal : ./lancer.sh).
  Il met le PC en mode d'energie "performance" le temps du jeu, si le systeme le permet.
- Le jeu demarre en plein ecran. Alt+Entree ou F11 : plein ecran / fenetre.
- Image plus fine mais plus lente : SIMPSONS_RESOLUTION=2 ./lancer.sh (1 = normal, 3 = 4K).

MANETTE
- Une manette Xbox (filaire ou Bluetooth) fonctionne directement.
- Clavier : fleches ou ZQSD pour se deplacer, Entree = START, Espace = A, E = B, R = X, F = Y.

EN CAS DE PROBLEME
- Le journal est dans ~/.local/share/les-simpson-le-jeu/jeu.log : l'envoyer avec une photo de l'ecran.
- Les sauvegardes sont dans ce meme dossier (sous-dossier "donnees").
FIN
# Oldest C library the program and libdxcompiler.so accept (2.35 = Ubuntu 22.04)
glibc=$(objdump -T "$out/les-simpson-le-jeu" "$out/libdxcompiler.so" | grep -oE "GLIBC_[0-9.]+" | sort -Vu | tail -1)
echo "Bibliotheque C demandee : $glibc (2.35 = Ubuntu 22.04)"
# Installed copy, apart from the work folder, where the menu entry points (the saves stay in ~/.local/share)
jeux="$HOME/Jeux/Les Simpson - Le Jeu"
mkdir -p "$jeux"
rsync -a --delete --exclude 'les-simpson-le-jeu.avant-*' --exclude userdata "$out/" "$jeux/"
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/les-simpson-le-jeu.desktop" << EOF
[Desktop Entry]
Type=Application
Name=Les Simpson - Le Jeu
Comment=The Simpsons Game (Xbox 360) recompilé
Exec="$jeux/lancer.sh"
Path=$jeux
Icon=$jeux/icone.png
Terminal=false
Categories=Game;
EOF
echo "Jeu installe : $jeux (raccourci dans le menu des applications, rubrique Jeux)"
