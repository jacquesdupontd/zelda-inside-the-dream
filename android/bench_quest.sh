#!/bin/bash
# Adapted from the existing qrun.sh: keep raw logs and isolate the player's saves.
set -euo pipefail
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
label=${1:?usage: bench_quest.sh label seconds [LARECOMP_NAME=value ...]}
seconds=${2:?missing duration}
shift 2
[[ "$label" =~ ^[a-zA-Z0-9_-]+$ ]] || exit 2
[[ "$seconds" =~ ^[0-9]+$ ]] || exit 2
serial=${ANDROID_SERIAL:?set ANDROID_SERIAL to the Quest device}
adbq=(adb -s "$serial")
data=/sdcard/Android/data/com.lasrecomp.quest/files
out=build/quest/$label
[ ! -e "$out/runtime.log" ] || { printf 'Results already exist: %s\n' "$out" >&2; exit 2; }
mkdir -p "$out"
envfile=$(mktemp)
logcat_pid=
cleanup() {
    "${adbq[@]}" shell am force-stop com.lasrecomp.quest >/dev/null 2>&1 || true
    "${adbq[@]}" shell rm -f "$data/env.txt" >/dev/null 2>&1 || true
    if [ -n "$logcat_pid" ]; then kill "$logcat_pid" 2>/dev/null || true; fi
    rm -f "$envfile"
}
trap cleanup EXIT
"${adbq[@]}" shell am force-stop com.lasrecomp.quest
"${adbq[@]}" shell "rm -rf '$data/save_gpt61sol_reference'; cp -R '$data/save' '$data/save_gpt61sol_reference'; chmod -R 777 '$data/save_gpt61sol_reference'"
{
    printf '%s\n' 'LARECOMP_INPUT=200:A:6,320:L:8,320:R:8,480:A:6,640:A:6,800:A:6,950:DOWN:6,1010:A:6,1120:A:4,1180:A:4'
    printf '%s\n' 'LARECOMP_WARP_ON_LOAD=Field:Field_15D' 'LARECOMP_FP=1' 'LARECOMP_NO_USER_INPUT=1'
    printf '%s\n' "LARECOMP_SAVE_DIR=$data/save_gpt61sol_reference" 'LARECOMP_VK_PROF=1' 'LARECOMP_LOG_PACE=1'
    printf '%s\n' 'LARECOMP_XR_SCENE_LOG=1'
    printf '%s\n' 'LARECOMP_WINDOW_SHOT=1500' "LARECOMP_SHOT_PATH=$data/build/gpt61sol-$label.png"
    for kv in "$@"; do
        [[ "$kv" =~ ^LARECOMP_[A-Z0-9_]+= ]] || { printf 'Invalid option: %s\n' "$kv" >&2; exit 2; }
        printf '%s\n' "$kv"
    done
} > "$envfile"
cp "$envfile" "$out/env.txt"
git rev-parse HEAD > "$out/source-head.txt"
git diff > "$out/source.diff"
installed_apk=$("${adbq[@]}" shell pm path com.lasrecomp.quest)
installed_apk=${installed_apk#package:}
installed_apk=${installed_apk//$'\r'/}
"${adbq[@]}" shell sha256sum "$installed_apk" > "$out/installed-apk.sha256"
"${adbq[@]}" push "$envfile" "$data/env.txt" >/dev/null
"${adbq[@]}" logcat -T 1 -v time VrApi:I '*:S' > "$out/vrapi.log" 2>&1 &
logcat_pid=$!
"${adbq[@]}" shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1 || true
"${adbq[@]}" shell am start -n com.lasrecomp.quest/android.app.NativeActivity > "$out/launch.txt"
sleep "$seconds"
"${adbq[@]}" pull "$data/lasrecomp.log" "$out/runtime.log" >/dev/null
"${adbq[@]}" pull "$data/build/gpt61sol-$label.png" "$out/scene.png" >/dev/null 2>&1 || true
"${adbq[@]}" shell dumpsys thermalservice > "$out/thermal.txt"
printf 'Raw benchmark results: %s\n' "$out"
