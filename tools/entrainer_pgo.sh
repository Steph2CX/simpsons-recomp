#!/bin/zsh
# Optimisation guidee par profil : construit la version instrumentee (simpsons/build-pgo-gen), la fait jouer
# 4 minutes sur 6 episodes, fusionne les mesures (portable/pgo/simpsons.profdata) puis reconstruit simpsons/build-pgo.
set -e
root="${0:A:h}/.."
cd "$root/simpsons"
cmake --build build-pgo-gen > /dev/null
rm -f "$root/portable/pgo/"*.profraw
export LLVM_PROFILE_FILE="$root/portable/pgo/%p.profraw" SIMPSONS_BUILD=build-pgo-gen SIMPSONS_DUREE=240 SIMPSONS_FENETRE=1 CAPTURES=99999
for batch in "EIGHTY_BITES MOB_RULES NEVERQUEST" "TREEHUGGER COLOSSAL_DONUT LAND_OF_CHOCOLATE"; do
    for ep in ${=batch}; do "$root/tools/test_episode.sh" $ep 270 & sleep 3; done
    wait
done
xcrun llvm-profdata merge -o "$root/portable/pgo/simpsons.profdata" "$root/portable/pgo/"*.profraw
cmake --build build-pgo > /dev/null
echo "build-pgo pret"
