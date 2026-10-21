#!/bin/zsh
# Lance le jeu avec un delai maximal (defaut 30 s), log dans recomp/run.log
cd "${0:A:h}"
./build/SimpsonsRecomp game/default.xex > recomp/run.log 2>&1 &
pid=$!
for i in $(seq 1 ${1:-30}); do kill -0 $pid 2>/dev/null || break; sleep 1; done
kill -9 $pid 2>/dev/null && echo "(arrete apres ${1:-30} s)"
wait $pid 2>/dev/null
echo "exit $?, $(wc -l < recomp/run.log) lignes"
