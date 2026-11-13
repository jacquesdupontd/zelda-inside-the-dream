#!/bin/sh
# Inside The Dream : son ecran d'accueil (jouer, jouer en VR quand le PC VR existe, les reglages), depuis ce dossier. Ses sauvegardes (save/), ses reglages (larecomp.ini), son code du jeu et ses
# caches restent ici, comme la version Windows a cote de lasrecomp.exe (LARECOMP_DATA_ROOT : runtime/paths.c).
D=$(cd "$(dirname "$0")" && pwd)
export LARECOMP_DATA_ROOT="$D"
# (sans FUSE, l'AppImage se decompresse dans /tmp a chaque lancement au lieu de se monter)
command -v fusermount >/dev/null 2>&1 || command -v fusermount3 >/dev/null 2>&1 || export APPIMAGE_EXTRACT_AND_RUN=1
cd "$D" && exec "$D/Inside The Dream.AppImage" extracted "$@"
