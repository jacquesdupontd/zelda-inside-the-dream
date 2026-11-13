#!/bin/bash
# pvbuild.sh (Raccoon Windows, MSYS2): the PCVR branch's release program, in F:\LASRecomp-pcvr (the PC game left alone)
exec > /f/LASRecomp-pcvr/pvbuild.log 2>&1
set -x
export MSYSTEM=UCRT64 PATH=/ucrt64/bin:/usr/bin:$PATH
cd /f/LASRecomp-pcvr
make dist RANDO_PY=/c/Users/Alloxrinfo/AppData/Local/Programs/Python/Python312/python.exe || { echo FAILED dist; exit 1; }
ls -la "build/dist/LAS Recomp/"
echo PVBUILD_DONE
