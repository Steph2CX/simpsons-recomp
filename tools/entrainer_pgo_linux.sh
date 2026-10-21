#!/usr/bin/env bash
# Linux version of entrainer_pgo.sh (profile-guided optimisation): builds the instrumented game (linux/build-pgo-gen),
# plays 4 episodes one after the other (5.5 min each: the laptop reaches gameplay later than the Mac; 720p, window,
# no sound, "performance" power profile), merges the counters (linux/pgo/simpsons.profdata),
# then builds linux/build-pgo. Does not touch linux/build nor the installed game; install with
#   cp linux/build-pgo/SimpsonsRecomp "linux/Les Simpson - Le Jeu (Linux)/les-simpson-le-jeu"
set -e
# Keep the PC awake (idle suspend stopped a training run)
# (KDE's own inhibitor: logind "sleep" inhibition needs a password when started over ssh)
[[ -z "$PGO_EVEIL" ]] && command -v kde-inhibit > /dev/null && exec env PGO_EVEIL=1 kde-inhibit --power "$0" "$@"
root="$(cd "$(dirname "$0")/.." && pwd)"
here="$root/linux"
jobs=$(nproc)
export CC=clang CXX=clang++
profdata=$(command -v llvm-profdata || ls /usr/bin/llvm-profdata-* | sort -V | tail -1)
pgo="$here/pgo"

configure() {  # configure <dir> <extra flags>
    cmake -S "$root/simpsons/runtime" -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_CXX_FLAGS=-march=x86-64-v3 $2" "-DCMAKE_EXE_LINKER_FLAGS=$3" \
        -DCMAKE_PREFIX_PATH="$here/prefix" -DXENON_RECOMP_BUILD="$here/build-xenon" > /dev/null
}

echo "== 1/4 Version instrumentee =="
configure "$here/build-pgo-gen" "-fprofile-instr-generate" "-fprofile-instr-generate"
nice cmake --build "$here/build-pgo-gen" -j "$jobs" --target SimpsonsRecomp

echo "== 2/4 Entrainement (4 episodes de 5 minutes et demie) =="
mkdir -p "$pgo"; rm -f "$pgo"/*.profraw
# Started without arguments, the game looks for game/ next to the program: run a copy inside the game folder
jeu="$here/Les Simpson - Le Jeu (Linux)"
cp "$here/build-pgo-gen/SimpsonsRecomp" "$jeu/les-simpson-le-jeu-pgo-gen"
export LLVM_PROFILE_FILE="$pgo/%p.profraw" SIMPSONS_BINAIRE="$jeu/les-simpson-le-jeu-pgo-gen" SIMPSONS_DUREE=330 SIMPSONS_RESOLUTION=1
profile=$(powerprofilesctl get 2> /dev/null || true)
powerprofilesctl set performance 2> /dev/null || true
for ep in EIGHTY_BITES NEVERQUEST TREEHUGGER COLOSSAL_DONUT; do
    "$root/tools/test_episode_linux.sh" $ep 360
done
[[ -n "$profile" ]] && powerprofilesctl set "$profile" 2> /dev/null || true
rm -f "$jeu/les-simpson-le-jeu-pgo-gen"
unset LLVM_PROFILE_FILE SIMPSONS_BINAIRE SIMPSONS_DUREE SIMPSONS_RESOLUTION

echo "== 3/4 Fusion des mesures =="
"$profdata" merge -o "$pgo/simpsons.profdata" "$pgo"/*.profraw

echo "== 4/4 Version optimisee =="
configure "$here/build-pgo" \
    "-fprofile-instr-use=$pgo/simpsons.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" ""
nice cmake --build "$here/build-pgo" -j "$jobs" --target SimpsonsRecomp
echo "build-pgo pret : $here/build-pgo/SimpsonsRecomp"
