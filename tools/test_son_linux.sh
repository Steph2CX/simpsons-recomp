#!/usr/bin/env bash
# test_son_linux.sh <EPISODE> [secondes] : like test_episode_linux.sh WITH sound; records the speakers (parec monitor)
# into linux/tests/son_<EPISODE>.f32 and counts the dropouts with analyse_son.py (after 120 s).
# window, no sound). Log in linux/tests/ep_<EPISODE>.log; SIMPSONS_BINAIRE=<exe> to test another build.
here="$(cd "$(dirname "$0")/.." && pwd)"
jeu="$here/linux/Les Simpson - Le Jeu (Linux)"
exe="${SIMPSONS_BINAIRE:-$jeu/les-simpson-le-jeu}"
out="$here/linux/tests"; mkdir -p "$out"
ep=$1; duree=${2:-240}
T=$(mktemp -d); mkdir -p "$T/d"
python3 - "$ep" "$T" "$jeu/game/simpsons_gameflow.lua" <<'P'
import sys,re
ep,T,src=sys.argv[1:4]
s=open(src,newline='').read()
s=s.replace('\tepisode:SetDefault()\r\n','',1)
m=re.search(r'episode = NewEpisode\( game, "'+ep+r'",[^\n]*\n',s)
s=s[:m.end()]+'\tepisode:SetDefault()\r\n'+s[m.end():]
open(T+'/d/simpsons_gameflow.lua','w',newline='').write(s)
P
keys="i1380:START,i1420:START,i1460:START,i1700:A,i1900:A,i2100:A,i2600:A,i3100:A,i3300:A"
fin=$((duree * 30))
for f in $(seq 3600 300 $fin); do keys="$keys,i$f:A"; done
for f in $(seq 6000 1200 $fin); do keys="$keys,i$f:AVANCE*200,i$((f+300)):X,i$((f+400)):VADROITE*150"; done
for f in $(seq 6600 900 $fin); do keys="$keys,i$f:Y,i$((f+60)):B,i$((f+120)):CAMGAUCHE*60,i$((f+200)):A,i$((f+215)):A,i$((f+500)):RT*30,i$((f+560)):RECULE*120,i$((f+700)):VAGAUCHE*100"; done
keys="$keys${TOUCHES_EN_PLUS:+,$TOUCHES_EN_PLUS}"
cd "$jeu" # the game data is passed explicitly: a build outside this folder (PGO) does not find it by itself
SIMPSONS_CONSOLE=1 SIMPSONS_DONNEES="$T" SIMPSONS_FENETRE=1 SIMPSONS_TOUCHES="$keys" \
  "$exe" "$jeu/game/default.xex" > "$out/son_$ep.log" 2>&1 &
pid=$!
parec --device=@DEFAULT_MONITOR@ --format=float32le --rate=48000 --channels=2 --raw > "$out/son_$ep.f32" &
rec=$!
sleep "$duree"; kill $rec; kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
rm -rf "$T"
# Average frame rate once in the episode (after 120 s), from the "images/s" lines
grep -oE "a [0-9.]+ s, [0-9.]+ images/s" "$out/son_$ep.log" | LC_ALL=C awk '$2 > 120 { n++; s += $4; if ($4 < 30) low++ } END { printf "%s : %.1f images/s en moyenne (%d mesures, %d sous 30)\n", "'"$ep"'", n ? s / n : 0, n, low }'
python3 "$here/tools/analyse_son.py" "$out/son_$ep.f32" 120
