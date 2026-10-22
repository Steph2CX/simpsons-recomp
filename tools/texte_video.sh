#!/bin/zsh
# Makes simpsons/runtime/texte_video.h: the message shown during movies, written by the game itself with the font and
# look of its "Appuyez sur START." (tools/texte_video.py). For each language, the game runs on a temporary copy of
# its texts where that title screen text is replaced by the message (and the copyright line emptied, which keeps the
# texts' size and clears the picture); its title screen is captured in 5K over a pulse of the text. The installed game
# and simpsons/modifications are never changed.
# Needs simpsons/build (development build), simpsons/game (French disc) and simpsons/game-us (American disc).
# Usage: tools/texte_video.sh
set -e
tools="${0:A:h}"
project="$tools/../simpsons"
work=$(mktemp -d)
trap 'rm -rf "$work" "$project"/recomp/texte_video_*.ppm' EXIT

# <language> <disc folder> <text file> <old> <new> [<old> <new>...] ("\n" in <new> = line break in the game's texts)
capture() {
    local language=$1 disc=$2 texts=$3
    shift 3
    local layout="$work/$language"
    mkdir -p "$layout/modifications/frontend/text" "$layout/donnees"
    ln -s "${disc:A}" "$layout/game"
    while (( $# >= 2 )); do
        print -r -- "$1"$'\t'"$2" >> "$layout/textes.tsv"
        shift 2
    done
    python3 -I "$tools/modifier_textes.py" "$disc/frontend/text/$texts" "$layout/modifications/frontend/text/$texts" \
        "$layout/textes.tsv" > /dev/null
    # From simpsons/ (its shader cache); captures in simpsons/recomp/texte_video_<language>_<frame>.ppm, removed after
    (cd "$project" && SIMPSONS_CAPTURE=$(seq -s, 700 2 732) SIMPSONS_CAPTURE_NOM="texte_video_${language}_" \
        SIMPSONS_FENETRE=1 SIMPSONS_RESOLUTION=4 SIMPSONS_DUREE=30 SIMPSONS_DONNEES="$layout/donnees" \
        ./build/SimpsonsRecomp "$layout/game/default.xex" > "$layout/journal.txt" 2>&1)
    files=$(ls "$project"/recomp/texte_video_${language}_*.ppm | tr '\n' ',' | sed 's/,$//')
    arguments+=("$language:$files")
}

arguments=()
capture en "$project/game-us" e172a05c.str \
    'Press START' 'Press Esc or Select\nto skip the video' \
    'Game software © 2007 Electronic Arts Inc. All Rights Reserved.' ' '
capture fr "$project/game" cae47a5a.str \
    'Appuyez sur START.' 'Appuyez sur Échap ou sur Select\npour passer la vidéo' \
    'Logiciel de jeu © 2007 Electronic Arts Inc. Tous droits réservés.' ' '
python3 -I "$tools/texte_video.py" "$project/runtime/texte_video.h" "${arguments[@]}"
