#!/bin/zsh
# Construit pour tvOS (Apple TV, tvOS 17 minimum) les bibliotheques dont le jeu a besoin, puis configure
# simpsons/build-tvos :
#   prefix/             SDL3 3.4.16 (statique, sans libusb) et FFmpeg 9.0.2 (decodeurs xma1/xma2 seulement)
#   build-xenon/        utilitaires de XenonRecomp (XenonUtils, disasm, fmt)
#   src/MoltenVK/       MoltenVK 1.4.2 (Vulkan sur Metal, statique)
#   build-dxc-tvos/     DXC (compilateur de shaders), meme version que celle du Mac (commit 416fab6b), avec
#                       correctifs/dxc-tvos.patch ; ses generateurs de code sont construits pour le Mac (build-dxc-hote)
# Long (environ 30 min). Chaque etape deja faite est sautee. TVOS_DEPS=<dossier> : autre dossier que tvos/.
set -e

racine="${0:A:h:h}"
tvos="${TVOS_DEPS:-$racine/tvos}"
mkdir -p "$tvos/src"
cible=(-DCMAKE_SYSTEM_NAME=tvOS -DCMAKE_OSX_SYSROOT=appletvos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0
    -DCMAKE_MACOSX_BUNDLE=OFF -DCMAKE_BUILD_TYPE=Release)
journal() { echo "$(date +%H:%M) $1"; }

if [[ ! -f "$tvos/prefix/lib/libSDL3.a" ]]; then
    journal "SDL3"
    [[ -d "$tvos/src/SDL" ]] || git clone -q --depth 1 --branch release-3.4.16 https://github.com/libsdl-org/SDL.git "$tvos/src/SDL"
    cmake -S "$tvos/src/SDL" -B "$tvos/build-sdl" -G Ninja $cible -DCMAKE_INSTALL_PREFIX="$tvos/prefix" -DSDL_SHARED=OFF \
        -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_HIDAPI_LIBUSB=OFF -DSDL_EXAMPLES=OFF -DSDL_TESTS=OFF > "$tvos/sdl.log"
    ninja -C "$tvos/build-sdl" install >> "$tvos/sdl.log"
fi

if [[ ! -f "$tvos/prefix/lib/libavcodec.a" ]]; then
    journal "FFmpeg"
    [[ -d "$tvos/src/ffmpeg" ]] || git clone -q --depth 1 --branch n9.0.2 https://git.ffmpeg.org/ffmpeg.git "$tvos/src/ffmpeg"
    sdk=$(xcrun --sdk appletvos --show-sdk-path)
    (cd "$tvos/src/ffmpeg" && ./configure --prefix="$tvos/prefix" --enable-static --disable-shared --disable-programs --disable-doc \
        --disable-autodetect --disable-network --disable-everything --enable-decoder='xma1,xma2' --disable-avformat \
        --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --arch=arm64 --target-os=darwin \
        --enable-cross-compile --cc="xcrun --sdk appletvos clang" --sysroot="$sdk" \
        --extra-cflags="-arch arm64 -mtvos-version-min=17.0" --extra-ldflags="-arch arm64 -mtvos-version-min=17.0" &&
        make -j8 install) > "$tvos/ffmpeg.log" 2>&1
fi

if [[ ! -f "$tvos/build-xenon/XenonUtils/libXenonUtils.a" ]]; then
    journal "XenonRecomp (utilitaires)"
    cmake -S "$racine/XenonRecomp" -B "$tvos/build-xenon" -G Ninja $cible -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$tvos/xenon.log"
    ninja -C "$tvos/build-xenon" XenonUtils disasm fmt >> "$tvos/xenon.log"
fi

if [[ ! -f "$tvos/src/MoltenVK/Package/Release/MoltenVK/static/MoltenVK.xcframework/tvos-arm64_arm64e/libMoltenVK.a" ]]; then
    journal "MoltenVK"
    [[ -d "$tvos/src/MoltenVK" ]] || git clone -q --depth 1 --branch v1.4.2 https://github.com/KhronosGroup/MoltenVK.git "$tvos/src/MoltenVK"
    (cd "$tvos/src/MoltenVK" && ./fetchDependencies --tvos && make tvos) > "$tvos/moltenvk.log" 2>&1
fi

if [[ ! -f "$tvos/build-dxc-tvos/lib/libdxcompiler.dylib" ]]; then
    journal "DXC"
    dxc="$tvos/src/dxc"
    if [[ ! -d "$dxc" ]]; then
        git init -q "$dxc"
        git -C "$dxc" remote add origin https://github.com/microsoft/DirectXShaderCompiler.git
        git -C "$dxc" fetch -q --depth 1 origin 416fab6b5c4ba956a320d9131102304da995edfc
        git -C "$dxc" -c advice.detachedHead=false checkout -q FETCH_HEAD
        git -C "$dxc" submodule update -q --init --recursive --depth 1
        git -C "$dxc" apply "$racine/correctifs/dxc-tvos.patch"
    fi
    commun=(-G Ninja -C "$dxc/cmake/caches/PredefinedParams.cmake" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DHLSL_SKIP_GENERATED_CHECK=ON
        -DSPIRV_BUILD_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_TESTS=OFF -DHLSL_INCLUDE_TESTS=OFF)
    # Generateurs de code (tablegen) pour le Mac, utilises pendant la construction pour tvOS
    cmake -S "$dxc" -B "$tvos/build-dxc-hote" $commun -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 > "$tvos/dxc.log" 2>&1
    ninja -C "$tvos/build-dxc-hote" llvm-tblgen clang-tblgen >> "$tvos/dxc.log"
    cmake -S "$dxc" -B "$tvos/build-dxc-tvos" $commun $cible -DLLVM_INCLUDE_EXAMPLES=OFF \
        -DLLVM_TABLEGEN="$tvos/build-dxc-hote/bin/llvm-tblgen" -DCLANG_TABLEGEN="$tvos/build-dxc-hote/bin/clang-tblgen" >> "$tvos/dxc.log" 2>&1
    ninja -C "$tvos/build-dxc-tvos" dxcompiler >> "$tvos/dxc.log"
fi

# Le jeu (code recompile et runtime), optimise avec le profil d'execution du Mac s'il existe
if [[ -z "$TVOS_DEPS" && ! -f "$racine/simpsons/build-tvos/build.ninja" ]]; then
    journal "configuration de simpsons/build-tvos"
    pgo="$racine/portable/pgo/simpsons.profdata"
    drapeaux=""
    [[ -f "$pgo" ]] && drapeaux="-fprofile-instr-use=$pgo -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
    cmake -S "$racine/simpsons/runtime" -B "$racine/simpsons/build-tvos" -G Ninja $cible -DSIMPSONS_TVOS=ON \
        -DXENON_RECOMP_BUILD="$tvos/build-xenon" "-DCMAKE_CXX_FLAGS=$drapeaux" -DCMAKE_OBJCXX_FLAGS=-fobjc-arc > /dev/null
fi
journal "dependances pretes dans $tvos"
