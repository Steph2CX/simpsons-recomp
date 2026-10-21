#!/bin/zsh
# Construit les bibliotheques statiques de la version Mac autonome (puce Apple, macOS 15 minimum), puis configure
# simpsons/build-portable :
#   prefix/        libusb 1.0.30, SDL3 3.4.16 (manettes USB par libusb integree) et FFmpeg 9.0.2 (decodeurs xma1/xma2)
#   build-xenon/   utilitaires de XenonRecomp (XenonUtils, disasm, fmt)
# MoltenVK vient de Homebrew (brew install molten-vk), lie en statique. Outils : brew install cmake ninja autoconf
# automake libtool pkg-config molten-vk. Chaque etape deja faite est sautee. PORTABLE_DEPS=<dossier> : autre dossier.
set -e

racine="${0:A:h:h}"
deps="${PORTABLE_DEPS:-$racine/portable}"
prefix="$deps/prefix"
mkdir -p "$deps/src"
mac=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0)
libusb=(-DLibUSB_LIBRARY="$prefix/lib/libusb-1.0.a" -DLibUSB_INCLUDE_PATH="$prefix/include/libusb-1.0"
    "-DLibUSB_LINK_LIBRARIES=-framework IOKit;-framework CoreFoundation;-framework Security")
journal() { echo "$(date +%H:%M) $1"; }

if [[ ! -f "$prefix/lib/libusb-1.0.a" ]]; then
    journal "libusb"
    [[ -d "$deps/src/libusb" ]] || git clone -q --depth 1 --branch v1.0.30 https://github.com/libusb/libusb.git "$deps/src/libusb"
    (cd "$deps/src/libusb" && ./bootstrap.sh && mkdir -p build && cd build &&
        MACOSX_DEPLOYMENT_TARGET=15.0 CFLAGS="-O2 -mmacosx-version-min=15.0 -arch arm64" ../configure --prefix="$prefix" \
            --disable-shared --enable-static && make -j8 install) > "$deps/libusb.log" 2>&1
fi

if [[ ! -f "$prefix/lib/libSDL3.a" ]]; then
    journal "SDL3"
    [[ -d "$deps/src/SDL" ]] || git clone -q --depth 1 --branch release-3.4.16 https://github.com/libsdl-org/SDL.git "$deps/src/SDL"
    cmake -S "$deps/src/SDL" -B "$deps/build-sdl" -G Ninja $mac -DCMAKE_INSTALL_PREFIX="$prefix" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
        -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DSDL_TESTS=OFF -DSDL_HIDAPI_LIBUSB=ON -DSDL_HIDAPI_LIBUSB_SHARED=OFF $libusb \
        > "$deps/sdl.log"
    ninja -C "$deps/build-sdl" install >> "$deps/sdl.log"
fi

if [[ ! -f "$prefix/lib/libavcodec.a" ]]; then
    journal "FFmpeg"
    [[ -d "$deps/src/ffmpeg" ]] || git clone -q --depth 1 --branch n9.0.2 https://git.ffmpeg.org/ffmpeg.git "$deps/src/ffmpeg"
    (cd "$deps/src/ffmpeg" && ./configure --prefix="$prefix" --enable-static --disable-shared --disable-programs --disable-doc \
        --disable-autodetect --disable-network --disable-everything --enable-decoder='xma1,xma2' --disable-avformat \
        --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --arch=arm64 \
        --extra-cflags='-mmacosx-version-min=15.0' --extra-ldflags='-mmacosx-version-min=15.0' && make -j8 install) \
        > "$deps/ffmpeg.log" 2>&1
fi

if [[ ! -f "$deps/build-xenon/XenonUtils/libXenonUtils.a" ]]; then
    journal "XenonRecomp (utilitaires)"
    cmake -S "$racine/XenonRecomp" -B "$deps/build-xenon" -G Ninja $mac "-DCMAKE_CXX_FLAGS=-include cstdlib" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$deps/xenon.log"
    ninja -C "$deps/build-xenon" XenonUtils disasm fmt >> "$deps/xenon.log"
fi

# Le jeu (code recompile et runtime), optimise avec le profil d'execution s'il existe (tools/entrainer_pgo.sh)
if [[ -z "$PORTABLE_DEPS" && ! -f "$racine/simpsons/build-portable/build.ninja" ]]; then
    journal "configuration de simpsons/build-portable"
    pgo="$deps/pgo/simpsons.profdata"
    drapeaux=""
    [[ -f "$pgo" ]] && drapeaux="-fprofile-instr-use=$pgo -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
    cmake -S "$racine/simpsons/runtime" -B "$racine/simpsons/build-portable" -G Ninja $mac -DSIMPSONS_PORTABLE=ON \
        -DXENON_RECOMP_BUILD="$deps/build-xenon" "-DCMAKE_CXX_FLAGS=$drapeaux" $libusb > /dev/null
fi
journal "dependances pretes dans $deps"
