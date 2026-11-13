#!/usr/bin/env bash
# tools/windows/itd_installer.sh <dist folder> <version> <out folder>: Inside The Dream's Windows installer and zip from a
# `make dist` folder (build/dist/LAS Recomp, built on Windows): <out>/InsideTheDream-<version>-Windows-Setup.exe (Inno
# Setup, tools/windows/itd.iss: one click, the start screen after) and <out>/InsideTheDream-<version>-Windows.zip (the
# same files, to unpack anywhere). Inno Setup 6's ISCC.exe: $ISCC, run through wine when this isn't Windows
# (WINEPREFIX, default ~/.wine-inno with Inno Setup in C:\InnoSetup6).
set -euo pipefail
DIST=$(realpath "${1:?usage: itd_installer.sh <dist folder> <version> <out folder>}")
VER=${2:?version}
OUT=$(realpath -m "${3:?out folder}")
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT"
NAME="InsideTheDream-$VER-Windows"
[ -f "$DIST/lasrecomp.exe" ] || { echo "no lasrecomp.exe in $DIST" >&2; exit 1; }

# the inner pages' corner image: the app's icon, at the two sizes Inno picks from (100 % and 200 %)
ART=$(mktemp -d)
python3 - "$ROOT/docs/icon/icon_1024.png" "$ART" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGBA")
for s in (58, 116):
    bg = Image.new("RGBA", (s, s), (243, 245, 249, 255))
    bg.alpha_composite(im.resize((s, s), Image.LANCZOS))
    bg.convert("RGB").save(f"{sys.argv[2]}/wizard-small-{s}.png")
PY

if command -v wine >/dev/null && [ "$(uname -s)" = Linux ]; then
    export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-inno}" WINEDEBUG=-all
    ISCC="${ISCC:-$WINEPREFIX/drive_c/InnoSetup6/ISCC.exe}"
    w() { winepath -w "$1"; }
    run() { wine "$ISCC" "$@"; }
else
    ISCC="${ISCC:-C:/Program Files (x86)/Inno Setup 6/ISCC.exe}"
    w() { cygpath -w "$1"; }
    run() { "$ISCC" "$@"; }
fi
run /Q /DStageDir="$(w "$DIST")" /DAppVersion="$VER" /DOutputDir="$(w "$OUT")" /DOutputName="$NAME-Setup" \
    /DIconFile="$(w "$ROOT/docs/icon/icon.ico")" \
    /DWizardSmall="$(w "$ART/wizard-small-116.png"),$(w "$ART/wizard-small-58.png")" "$(w "$ROOT/tools/windows/itd.iss")"
rm -rf "$ART"

# the zip: the same folder, named for the app
STAGE=$(mktemp -d)
cp -R "$DIST" "$STAGE/Inside The Dream"
rm -f "$OUT/$NAME.zip"
(cd "$STAGE" && zip -qr "$OUT/$NAME.zip" "Inside The Dream")
rm -rf "$STAGE"
ls -la "$OUT/$NAME-Setup.exe" "$OUT/$NAME.zip"
