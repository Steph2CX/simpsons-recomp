#!/bin/zsh
# Makes simpsons/runtime/texte_video.h: the message shown during movies, written by the game itself in one of its
# translucent clouds (tools/texte_video.py). For each language, a new game starts at Land of Chocolate on a temporary
# copy of the texts where the "Nouvel objectif !" hint, shown in a cloud at the start of the level, is replaced by the
# message; the same moment is captured in 4K with the cloud, then without it (SIMPSONS_SANS_BULLES=1). The installed
# game and simpsons/modifications are never changed.
# Needs simpsons/build (development build), simpsons/game (French disc) and simpsons/game-us (American disc); about
# 3 minutes per capture, the four run two at a time.
# Usage: tools/texte_video.sh
set -e
tools="${0:A:h}"
project="$tools/../simpsons"
work=$(mktemp -d)
trap 'rm -rf "$work" "$project"/recomp/texte_video_*.ppm' EXIT

# <language> <disc folder> <text file> <old> <new> ("\n" in <new> = line break in the game's texts)
capture() {
    local language=$1 disc=$2 texts=$3 old=$4 new=$5 cloud
    local layout="$work/$language"
    mkdir -p "$layout"
    print -r -- "$old"$'\t'"$new" > "$layout/textes.tsv"
    for cloud in avec sans; do
        local data="$layout/$cloud"
        mkdir -p "$data/d"
        # New game starting at Land of Chocolate (as tools/test_episode.sh)
        python3 -I - "$disc/simpsons_gameflow.lua" "$data/d/simpsons_gameflow.lua" <<'P'
import re, sys
s = open(sys.argv[1], newline='').read().replace('\tepisode:SetDefault()\r\n', '', 1)
m = re.search(r'episode = NewEpisode\( game, "LAND_OF_CHOCOLATE",[^\n]*\n', s)
open(sys.argv[2], 'w', newline='').write(s[:m.end()] + '\tepisode:SetDefault()\r\n' + s[m.end():])
P
        # The hint is in the menus' texts, the characters' and the level's
        for folder in frontend simpsons_chars loc; do
            python3 -I "$tools/modifier_textes.py" "$disc/$folder/text/$texts" "$data/d/$folder/text/$texts" \
                "$layout/textes.tsv" 2> /dev/null
        done
        # From simpsons/ (its shader cache); capture in simpsons/recomp/texte_video_<language>_<cloud>_5000.ppm
        (cd "$project" && env $([[ $cloud == sans ]] && print SIMPSONS_SANS_BULLES=1) SIMPSONS_CAPTURE=5000 \
            SIMPSONS_CAPTURE_NOM="texte_video_${language}_${cloud}_" SIMPSONS_FENETRE=1 SIMPSONS_RESOLUTION=3 \
            SIMPSONS_MUET=1 SIMPSONS_DUREE=175 SIMPSONS_DONNEES="$data" \
            SIMPSONS_TOUCHES="i1380:START,i1420:START,i1460:START,i1700:A,i1900:A,i2100:A,i2600:A,i3100:A,i3300:A,i3600:A,i3900:A,i4200:A,i4500:A" \
            ./build/SimpsonsRecomp "${disc:A}/default.xex" > "$data/journal.txt" 2>&1 || true)
    done
}

capture en "$project/game-us" e172a05c.str \
    'New Objective!  When you see this icon, press [Select] to bring up your To Do List.' \
    'Press Esc or Select\nto skip the video' &
capture fr "$project/game" cae47a5a.str \
    'Nouvel objectif ! Quand vous voyez cette icône, appuyez sur [Select] pour faire apparaître votre liste de choses à faire.' \
    'Appuyez sur Échap ou sur Select\npour passer la vidéo' &
wait
recomp="$project/recomp"
python3 -I "$tools/texte_video.py" "$project/runtime/texte_video.h" \
    "en:$recomp/texte_video_en_avec_5000.ppm,$recomp/texte_video_en_sans_5000.ppm" \
    "fr:$recomp/texte_video_fr_avec_5000.ppm,$recomp/texte_video_fr_sans_5000.ppm"
