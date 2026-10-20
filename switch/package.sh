#!/bin/bash
# Stages the SD card's files in build-switch/sdcard/: switch/sheepdogwolf/ with the .nro and environment.txt.
#
#   switch/package.sh                     the game's files only; copy your game's data in yourself
#   switch/package.sh --data <folder>     also copies Levels, Musics, Voices, References and Bonus from <folder>,
#                                         your own copy of the PC game (a release must never include them)
set -e
cd "$(dirname "$0")/.."
data=""
if [ "$1" = "--data" ]; then
    data="$2"
    [ -d "$data/Levels" ] || { echo "$data has no Levels/ folder: not the PC game's folder" >&2; exit 1; }
fi
[ -f build-switch/SheepD3D.nro ] || { echo "build-switch/SheepD3D.nro is missing: run switch/build.sh first" >&2; exit 1; }

out=build-switch/sdcard/switch/sheepdogwolf
mkdir -p "$out"
cp build-switch/SheepD3D.nro "$out/sheepdogwolf.nro"
# the settings, unless a staged one was already edited
[ -f "$out/environment.txt" ] || cp switch/environment.txt "$out/environment.txt"
if [ -n "$data" ]; then
    for dir in Levels Musics Voices References Bonus Languages; do
        [ -d "$data/$dir" ] && cp -r "$data/$dir" "$out/"
    done
fi
echo "Staged $out: copy build-switch/sdcard/ to the root of the SD card."
