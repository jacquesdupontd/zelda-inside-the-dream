@echo off
cd /d F:\
del F:\LAS-Recomp-Windows.zip 2>nul
del F:\LASRecomp-fwin\pzip.done 2>nul
tar -a -c --options zip:compression=store -f F:\LAS-Recomp-Windows.zip --exclude "LAS Recomp/save_test" --exclude "LAS Recomp/test.ini" --exclude "LAS Recomp/cache" --exclude "*.log" --exclude "fpwarp-dump-*" "LAS Recomp" > F:\LASRecomp-fwin\pzip.out 2>&1
echo %ERRORLEVEL% > F:\LASRecomp-fwin\pzip.done
