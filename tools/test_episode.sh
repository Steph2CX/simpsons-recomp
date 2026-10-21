#!/bin/zsh
# test_episode.sh <EPISODE> [secondes] : new game starting at that episode, captures recomp/ep_<EPISODE>_<frame>.ppm
cd "${0:A:h}/../simpsons"
S=${TMPDIR:-/tmp}
ep=$1; T=$S/donnees_$ep
rm -rf $T; mkdir -p $T/d
python3 - "$ep" "$T" <<'P'
import sys,re
ep,T=sys.argv[1:3]
s=open('game/simpsons_gameflow.lua',newline='').read()
s=s.replace('\tepisode:SetDefault()\r\n','',1)
m=re.search(r'episode = NewEpisode\( game, "'+ep+r'",[^\n]*\n',s)
s=s[:m.end()]+'\tepisode:SetDefault()\r\n'+s[m.end():]
open(T+'/d/simpsons_gameflow.lua','w',newline='').write(s)
P
keys="i1380:START,i1420:START,i1460:START,i1700:A,i1900:A,i2100:A,i2600:A,i3100:A,i3300:A"
duree=${2:-380}; fin=$((duree * 30))
for f in $(seq 3600 300 $fin); do keys="$keys,i$f:A"; done
for f in $(seq 6000 1200 $fin); do keys="$keys,i$f:AVANCE*200,i$((f+300)):X,i$((f+400)):VADROITE*150"; done
for f in $(seq 6600 900 $fin); do keys="$keys,i$f:Y,i$((f+60)):B,i$((f+120)):CAMGAUCHE*60,i$((f+200)):A,i$((f+215)):A,i$((f+500)):RT*30,i$((f+560)):RECULE*120,i$((f+700)):VAGAUCHE*100"; done
keys="$keys${TOUCHES_EN_PLUS:+,$TOUCHES_EN_PLUS}"
SIMPSONS_DONNEES=$T SIMPSONS_CAPTURE_NOM=ep_${ep}_ SIMPSONS_CAPTURE=${CAPTURES:-4500,6000,7500,9000,10500} SIMPSONS_TOUCHES="$keys" SIMPSONS_MUET=1 \
  ./${SIMPSONS_BUILD:-build-portable}/SimpsonsRecomp game/default.xex > recomp/ep_$ep.log 2>&1 &
pid=$!
# Watchdog: a log silent for 15 s means a freeze; the threads' stacks go to recomp/ep_<EPISODE>_blocage.txt
(
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        age=$(( $(date +%s) - $(stat -f %m recomp/ep_$ep.log) ))
        if (( age > 15 )) && kill -0 $pid 2>/dev/null; then
            echo "[test] blocage detecte (journal muet depuis ${age} s)" >> recomp/ep_$ep.log
            sample $pid 3 -file recomp/ep_${ep}_blocage.txt > /dev/null 2>&1
            break
        fi
    done
) &
sleep ${2:-380}; kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
rm -rf $T
