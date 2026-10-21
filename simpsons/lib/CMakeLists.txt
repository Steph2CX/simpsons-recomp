cmake_minimum_required(VERSION 3.20)
project(SimpsonsRecompLib CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XENON_RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../XenonRecomp")
# Recompiled game code: ppc (all registers in PPCContext) or ppc-opt (simpsons_opt.toml: registers kept in local
# variables, much faster)
set(SIMPSONS_PPC_DIR "ppc" CACHE STRING "Folder of the recompiled code")
file(GLOB PPC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../${SIMPSONS_PPC_DIR}/*.cpp")

add_library(SimpsonsRecompLib STATIC ${PPC_SOURCES})

target_include_directories(SimpsonsRecompLib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../${SIMPSONS_PPC_DIR}"
    "${XENON_RECOMP_ROOT}/thirdparty/simde")

target_compile_options(SimpsonsRecompLib PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/recomp_overrides.h"
    -fno-strict-aliasing
    -Wno-switch
    -Wno-unused-function
    -Wno-unused-label
    -Wno-unused-variable)
