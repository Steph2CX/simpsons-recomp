cmake_minimum_required(VERSION 3.20)
project(SimpsonsRecomp CXX)
if(APPLE)
    enable_language(OBJCXX)
elseif(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SIMPSONS_TVOS=ON (Apple TV, with -DCMAKE_SYSTEM_NAME=tvOS, tvos/construire.sh): dependencies built for tvOS
# in tvos/ (SDL3, FFmpeg, MoltenVK, DXC)
option(SIMPSONS_TVOS "Apple TV build" OFF)
set(SIMPSONS_TVOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../tvos")

set(XENON_RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../XenonRecomp")
set(XENON_RECOMP_BUILD "${XENON_RECOMP_ROOT}/build" CACHE PATH "XenonRecomp build (static helpers)")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lib" SimpsonsRecompLib)

# Shader translation: XenosRecomp's recompiler + DXC, built apart (it has its own be<T> type)
set(XENOS_RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../XenosRecomp")
set(XENOS_THIRDPARTY "${XENOS_RECOMP_ROOT}/thirdparty")
file(READ "${XENOS_RECOMP_ROOT}/XenosRecomp/shader_common.h" SHADER_COMMON_TEXT)
# Read at configure time: a change to the header must re-run CMake, or the program keeps embedding the old text
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${XENOS_RECOMP_ROOT}/XenosRecomp/shader_common.h")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shader_common_header.cpp"
    "extern const char g_shaderCommonHeader[] = R\"__XENOS__(${SHADER_COMMON_TEXT})__XENOS__\";\n")

add_library(XenosTranslate STATIC
    xenos/xenos_translate.cpp
    xenos/hlsl_compile.cpp
    "${XENOS_RECOMP_ROOT}/XenosRecomp/shader_recompiler.cpp"
    "${XENOS_RECOMP_ROOT}/XenosRecomp/dxc_compiler.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/shader_common_header.cpp")
set_target_properties(XenosTranslate PROPERTIES CXX_STANDARD 17)
if(WIN32)
    target_sources(XenosTranslate PRIVATE windows/dxc_uuids.cpp)
endif()
target_include_directories(XenosTranslate PRIVATE
    "${XENOS_RECOMP_ROOT}/XenosRecomp"
    "${XENOS_THIRDPARTY}/dxc-bin/inc"
    "${XENOS_THIRDPARTY}/fmt/include"
    "${XENOS_THIRDPARTY}/smol-v/source"
    "${XENOS_THIRDPARTY}/xxHash"
    "${XENOS_THIRDPARTY}/zstd/lib")
target_precompile_headers(XenosTranslate PRIVATE "${XENOS_RECOMP_ROOT}/XenosRecomp/pch.h")
target_compile_options(XenosTranslate PRIVATE "SHELL:-include cstdlib" "SHELL:-include algorithm" -Wno-switch -Wno-unused-variable -fms-extensions)
if(SIMPSONS_TVOS)
    set(DXC_LIBRARY "${SIMPSONS_TVOS_ROOT}/build-dxc-tvos/lib/libdxcompiler.dylib")
elseif(WIN32)
    set(DXC_LIBRARY "${XENOS_THIRDPARTY}/dxc-bin/lib/x64/dxcompiler.lib")
elseif(NOT APPLE)
    set(DXC_LIBRARY "${XENOS_THIRDPARTY}/dxc-bin/lib/x64/libdxcompiler.so")
else()
    set(DXC_LIBRARY "${XENOS_THIRDPARTY}/dxc-bin/lib/arm64/libdxcompiler.dylib")
endif()
target_link_libraries(XenosTranslate PUBLIC
    "${DXC_LIBRARY}"
    "${XENON_RECOMP_BUILD}/thirdparty/fmt/libfmt.a")
set_target_properties(XenosTranslate PROPERTIES BUILD_RPATH "${XENOS_THIRDPARTY}/dxc-bin/lib/arm64")

# Hash of the shader translator (sources + DXC): names the folder of the translated shader cache (renderer.cpp)
set(SHADER_TRANSLATOR_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/xenos/xenos_translate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/xenos/xenos_translate.h"
    "${XENOS_RECOMP_ROOT}/XenosRecomp/shader_recompiler.cpp"
    "${XENOS_RECOMP_ROOT}/XenosRecomp/shader_recompiler.h"
    "${XENOS_RECOMP_ROOT}/XenosRecomp/shader_common.h"
    "${XENOS_RECOMP_ROOT}/XenosRecomp/shader.h"
    "${XENOS_RECOMP_ROOT}/XenosRecomp/shader_code.h"
    "${XENOS_RECOMP_ROOT}/XenosRecomp/constant_table.h"
    "${XENOS_RECOMP_ROOT}/XenosRecomp/dxc_compiler.cpp"
    "${XENOS_RECOMP_ROOT}/XenosRecomp/dxc_compiler.h"
    "${DXC_LIBRARY}")
string(JOIN "," SHADER_TRANSLATOR_LIST ${SHADER_TRANSLATOR_FILES})
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shader_translator_version.h"
    COMMAND ${CMAKE_COMMAND} "-DFILES=${SHADER_TRANSLATOR_LIST}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/shader_translator_version.h"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/empreinte_traducteur.cmake"
    DEPENDS ${SHADER_TRANSLATOR_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/empreinte_traducteur.cmake"
    VERBATIM)

add_executable(xenos_test xenos/xenos_test.cpp)
target_link_libraries(xenos_test PRIVATE XenosTranslate)
set_target_properties(xenos_test PROPERTIES BUILD_RPATH "${XENOS_THIRDPARTY}/dxc-bin/lib/arm64")

add_executable(SimpsonsRecomp
    main.cpp
    kernel.cpp
    threads.cpp
    gpu.cpp
    files.cpp
    xam.cpp
    xma.cpp
    audio_out.cpp
    shaders.cpp
    renderer.cpp
    gpu_draw.cpp
    textures.cpp
    input.cpp
    content.cpp
    debug_hooks.cpp
    game_fixes.cpp
    frame_pacing.cpp
    native_functions.cpp
    import_stubs.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/shader_translator_version.h")
target_include_directories(SimpsonsRecomp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(WIN32)
    target_sources(SimpsonsRecomp PRIVATE pc_window.cpp windows/simpsons.rc)
elseif(SIMPSONS_TVOS)
    target_sources(SimpsonsRecomp PRIVATE tvos_window.mm)
elseif(APPLE)
    target_sources(SimpsonsRecomp PRIVATE mac_window.mm)
else()
    target_sources(SimpsonsRecomp PRIVATE pc_window.cpp)
endif()

# SIMPSONS_PORTABLE=ON: SDL3, FFmpeg (XMA decoders only) and MoltenVK linked statically from SIMPSONS_DEPS
# (built by tools/construire_dependances.sh), so the application runs on any Apple silicon Mac (macOS 15+)
option(SIMPSONS_PORTABLE "Self-contained build" OFF)
set(SIMPSONS_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../../portable/prefix" CACHE PATH "Static dependencies")
find_package(PkgConfig REQUIRED)
if(WIN32)
    # Windows (cross-compiled with llvm-mingw, windows/construire.sh): static SDL3 and FFmpeg from windows/prefix,
    # Vulkan through the system loader (vulkan-1.dll, installed with the graphics driver)
    set(SIMPSONS_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../../windows/prefix" CACHE PATH "Static dependencies" FORCE)
    list(PREPEND CMAKE_PREFIX_PATH "${SIMPSONS_DEPS}")
elseif(SIMPSONS_TVOS)
    set(SIMPSONS_DEPS "${SIMPSONS_TVOS_ROOT}/prefix" CACHE PATH "Static dependencies" FORCE)
    set(SDL3_DIR "${SIMPSONS_DEPS}/lib/cmake/SDL3")
elseif(SIMPSONS_PORTABLE)
    list(PREPEND CMAKE_PREFIX_PATH "${SIMPSONS_DEPS}")
else()
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
endif()
find_package(SDL3 REQUIRED)
if(SIMPSONS_TVOS)
    # Vulkan headers shipped with MoltenVK (no Vulkan loader: MoltenVK linked directly)
    set(MOLTENVK_PACKAGE "${SIMPSONS_TVOS_ROOT}/src/MoltenVK/Package/Release/MoltenVK")
    set(Vulkan_INCLUDE_DIRS "${MOLTENVK_PACKAGE}/include")
elseif(NOT WIN32)
    find_package(Vulkan REQUIRED)
endif()

target_include_directories(SimpsonsRecomp PRIVATE
    "${XENON_RECOMP_ROOT}/XenonUtils"
    "${XENON_RECOMP_ROOT}/thirdparty/fmt/include"
    "${XENON_RECOMP_ROOT}/thirdparty/xxHash")

target_compile_options(SimpsonsRecomp PRIVATE
    -include cstdlib
    -fno-strict-aliasing
    -Wno-unused-function
    -Wno-asm-operand-widths)

target_link_libraries(SimpsonsRecomp PRIVATE
    SimpsonsRecompLib
    XenosTranslate
    "${XENON_RECOMP_BUILD}/XenonUtils/libXenonUtils.a"
    "${XENON_RECOMP_BUILD}/thirdparty/disasm/libdisasm.a"
    "${XENON_RECOMP_BUILD}/thirdparty/fmt/libfmt.a")

if(WIN32)
    target_compile_definitions(SimpsonsRecomp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _FILE_OFFSET_BITS=64)
    target_compile_definitions(XenosTranslate PRIVATE NOMINMAX)
    target_include_directories(SimpsonsRecomp PRIVATE "${SIMPSONS_DEPS}/include")
    target_include_directories(XenosTranslate PRIVATE "${SIMPSONS_DEPS}/include")
    target_link_libraries(SimpsonsRecomp PRIVATE "${SIMPSONS_DEPS}/lib/libavcodec.a" "${SIMPSONS_DEPS}/lib/libavutil.a"
        SDL3::SDL3-static "${SIMPSONS_DEPS}/lib/libvulkan-1.a" winmm bcrypt)
    # Window program (no console), large stack for the deeply nested recompiled calls of guest threads,
    # everything static except the system DLLs, dxcompiler.dll and vulkan-1.dll
    target_link_options(SimpsonsRecomp PRIVATE -mwindows "-Wl,--stack,16777216" -static)
elseif(SIMPSONS_TVOS)
    target_compile_definitions(SimpsonsRecomp PRIVATE SIMPSONS_TVOS=1)
    target_include_directories(SimpsonsRecomp PRIVATE ${Vulkan_INCLUDE_DIRS} "${SIMPSONS_DEPS}/include")
    target_link_libraries(SimpsonsRecomp PRIVATE "${SIMPSONS_DEPS}/lib/libavcodec.a" "${SIMPSONS_DEPS}/lib/libavutil.a"
        SDL3::SDL3-static "${MOLTENVK_PACKAGE}/static/MoltenVK.xcframework/tvos-arm64_arm64e/libMoltenVK.a"
        "-framework AudioToolbox" "-framework AVFoundation" "-framework CoreAudio" "-framework CoreVideo" "-framework CoreMedia"
        "-framework Metal" "-framework IOSurface" "-framework QuartzCore" "-framework CoreGraphics" "-framework Foundation"
        "-framework UIKit" "-framework GameController" "-framework CoreHaptics" "-framework CoreBluetooth" "-framework Security"
        objc)
    # DXC is shipped in the application's Frameworks folder (the program is at the root of the .app)
    set_target_properties(SimpsonsRecomp PROPERTIES BUILD_RPATH "@executable_path/Frameworks" MACOSX_BUNDLE OFF)
elseif(APPLE)
    set_target_properties(SimpsonsRecomp PROPERTIES BUILD_RPATH "${XENOS_THIRDPARTY}/dxc-bin/lib/arm64")
    target_link_libraries(SimpsonsRecomp PRIVATE "-framework AudioToolbox" "-framework AppKit" "-framework IOKit" objc)
else()
    # Linux: system SDL3, FFmpeg and Vulkan loader; libdxcompiler.so next to the program (or in the source tree)
    set_target_properties(SimpsonsRecomp xenos_test PROPERTIES BUILD_RPATH "$ORIGIN;${XENOS_THIRDPARTY}/dxc-bin/lib/x64")
    target_link_libraries(SimpsonsRecomp PRIVATE pthread dl rt m)
    # Runs on older distributions (glibc 2.35, Ubuntu 22.04): C++ runtime linked in, recent glibc symbol versions
    # redirected to their oldest version (linux_compat.cpp), unused shared libraries (libatomic from FFmpeg) dropped
    target_sources(SimpsonsRecomp PRIVATE linux_compat.cpp)
    target_link_options(SimpsonsRecomp PRIVATE -static-libstdc++ -static-libgcc -Wl,--as-needed -Wl,--exclude-libs,ALL
        "-Wl,--wrap=fmod,--wrap=fmodf,--wrap=sqrtf,--wrap=acosf,--wrap=asinf,--wrap=atan2f,--wrap=log10f,--wrap=sinh,--wrap=cosh"
        "-Wl,--wrap=__isoc23_strtol,--wrap=__isoc23_strtoll,--wrap=__isoc23_strtoul,--wrap=__isoc23_strtoull"
        "-Wl,--wrap=__isoc23_wcstol,--wrap=__isoc23_sscanf,--wrap=__isoc23_vsscanf,--wrap=__isoc23_fscanf"
        "-Wl,--wrap=strlcpy,--wrap=strlcat,--wrap=wcslcpy,--wrap=wcslcat,--wrap=arc4random")
endif()

if(WIN32 OR SIMPSONS_TVOS)
elseif(SIMPSONS_PORTABLE)
    find_library(MOLTENVK_STATIC libMoltenVK.a PATHS /opt/homebrew/opt/molten-vk/lib REQUIRED)
    target_include_directories(SimpsonsRecomp PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_include_directories(SimpsonsRecomp PRIVATE "${SIMPSONS_DEPS}/include")
    target_link_libraries(SimpsonsRecomp PRIVATE "${SIMPSONS_DEPS}/lib/libavcodec.a" "${SIMPSONS_DEPS}/lib/libavutil.a"
        "-framework CoreVideo" "-framework CoreMedia")
    target_link_libraries(SimpsonsRecomp PRIVATE SDL3::SDL3-static "${MOLTENVK_STATIC}"
        "-framework Metal" "-framework IOSurface" "-framework QuartzCore" "-framework CoreGraphics"
        "-framework IOKit" "-framework Foundation" "-framework AppKit" "-framework Security")
    # DXC is shipped next to the program, in Contents/Frameworks
    set_target_properties(SimpsonsRecomp PROPERTIES BUILD_RPATH "@executable_path/../Frameworks;${XENOS_THIRDPARTY}/dxc-bin/lib/arm64")
else()
    target_link_libraries(SimpsonsRecomp PRIVATE PkgConfig::FFMPEG SDL3::SDL3 Vulkan::Vulkan)
endif()

# __llvm_profile_write_file is only defined in the profile-instrumented build (PGO training)
if(APPLE)
    target_link_options(SimpsonsRecomp PRIVATE "-Wl,-U,___llvm_profile_write_file")
endif()
