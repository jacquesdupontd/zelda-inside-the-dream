#!/bin/bash
# pvgame.sh (Raccoon Windows, MSYS2): the game code (lasgame.dll) for the PCVR branch, from the game files of F:\LAS Recomp
exec > /f/LASRecomp-pcvr/pvgame.log 2>&1
set -x
export MSYSTEM=UCRT64 PATH=/ucrt64/bin:/usr/bin:$PATH
cd /f/LASRecomp-pcvr
[ -e extracted ] || cmd //c mklink //J 'F:\LASRecomp-pcvr\extracted' 'F:\LAS Recomp\extracted'
ls extracted | head
PY=/c/Users/Alloxrinfo/AppData/Local/Programs/Python/Python312/python.exe
time make gen RANDO_PY=$PY || { echo FAILED gen; exit 1; }
time make -j12 B=build/win-release DEPS=/f/LASRecomp-pcvr/build/deps-win RELEASE=1 RANDO_PY=$PY build/win-release/lasgame.dll || { echo FAILED gamelib; exit 1; }
ls -la build/win-release/lasgame.dll*
echo PVGAME_DONE
