#!/bin/bash
# Apply a measured runtime preset without changing the player's settings or saves.
set -euo pipefail
profile=${1:?usage: quest_profile.sh quality|appsw|appsw-dynamic|sgsr2|fidelity|fidelity90|fidelity90-hq|fidelity90-balanced [--print]}
case "$profile" in
    quality)
        values=$'LARECOMP_XR_SCREEN=0\nLARECOMP_XR_HZ=120\nLARECOMP_XR_EYE_SCALE=1.5\nLARECOMP_XR_APPSW=0\nLARECOMP_GAME_30=1\nLARECOMP_VRS=0'
        ;;
    appsw)
        values=$'LARECOMP_XR_SCREEN=0\nLARECOMP_XR_HZ=120\nLARECOMP_XR_EYE_SCALE=0.55\nLARECOMP_XR_OUTPUT_SCALE=0.9\nLARECOMP_XR_APPSW=1\nLARECOMP_GAME_30=0\nLARECOMP_FLOW_STRUCTURED=1\nLARECOMP_VRS=0'
        ;;
    fidelity|fidelity90|fidelity90-hq|fidelity90-balanced)
        values=$'LARECOMP_XR_SCREEN=0\nLARECOMP_XR_HZ=120\nLARECOMP_XR_EYE_SCALE=0.6\nLARECOMP_XR_OUTPUT_SCALE=0.9\nLARECOMP_XR_APPSW=1\nLARECOMP_GAME_30=0\nLARECOMP_FLOW_STRUCTURED=1\nLARECOMP_VRS=0\nLARECOMP_DYNRES_MIN=0.7\nLARECOMP_SGSR2=0\nLARECOMP_XR_SPATIAL_FSR=1\nLARECOMP_TEXTURE_ANISO=4\nLARECOMP_XR_FRESH_ONLY=1\nLARECOMP_DYNRES_FILE=build/gpt61sol-textures-fsr-efficient.txt'
        if [[ "$profile" != fidelity ]]; then
            # Experimental: frame budget only, NOT a simulation-speed fix.
            # Qualify real gameplay speed before treating 45->90 as playable.
            values=${values/LARECOMP_XR_HZ=120/LARECOMP_XR_HZ=90}
            values=${values/gpt61sol-textures-fsr-efficient.txt/gpt61sol-fidelity90.txt}
            values+=$'\nLARECOMP_DYNRES_TARGET_FPS=45\nLARECOMP_FP_DETAIL_LATE_MS=22.533333\nLARECOMP_FP_REACH_LATE_MS=23.733333'
            if [[ "$profile" == fidelity90-hq ]]; then
                # Candidate, not a measured optimum: +36% central-source pixels.
                values=${values/LARECOMP_XR_EYE_SCALE=0.6/LARECOMP_XR_EYE_SCALE=0.7}
                values=${values/LARECOMP_DYNRES_MIN=0.7/LARECOMP_DYNRES_MIN=0.8}
                values=${values/gpt61sol-fidelity90.txt/gpt61sol-fidelity90-hq.txt}
            fi
            if [[ "$profile" == fidelity90-balanced ]]; then
                # The current engine still needs 60 updates/renders per second
                # for normal game speed, while OpenXR samples 45 for AppSW90.
                # Higher ceiling indoors, same absolute resolution floor as120.
                values=${values/LARECOMP_XR_EYE_SCALE=0.6/LARECOMP_XR_EYE_SCALE=0.7}
                values=${values/LARECOMP_DYNRES_MIN=0.7/LARECOMP_DYNRES_MIN=0.6}
                values=${values/LARECOMP_DYNRES_TARGET_FPS=45/LARECOMP_DYNRES_TARGET_FPS=60}
                values=${values/LARECOMP_FP_DETAIL_LATE_MS=22.533333/LARECOMP_FP_DETAIL_LATE_MS=16.9}
                values=${values/LARECOMP_FP_REACH_LATE_MS=23.733333/LARECOMP_FP_REACH_LATE_MS=17.8}
                values=${values/gpt61sol-fidelity90.txt/gpt61sol-fidelity90-balanced.txt}
            fi
        fi
        ;;
    appsw-dynamic|sgsr2)
        values=$'LARECOMP_XR_SCREEN=0\nLARECOMP_XR_HZ=120\nLARECOMP_XR_OUTPUT_SCALE=0.9\nLARECOMP_XR_APPSW=1\nLARECOMP_GAME_30=0\nLARECOMP_FLOW_STRUCTURED=1\nLARECOMP_VRS=0\nLARECOMP_DYNRES_MIN=0.6'
        if [[ "$profile" == sgsr2 ]]; then
            values+=$'\nLARECOMP_XR_EYE_SCALE=0.6\nLARECOMP_SGSR2=1\nLARECOMP_SGSR2_KERNEL=1\nLARECOMP_SGSR2_SMAA=1\nLARECOMP_XR_FRESH_ONLY=1\nLARECOMP_DYNRES_FILE=build/quest-sgsr2-efficient-dynres.txt'
        else
            values+=$'\nLARECOMP_XR_EYE_SCALE=0.7\nLARECOMP_SGSR2=0\nLARECOMP_DYNRES_FILE=build/quest-appsw-dynres.txt'
        fi
        ;;
    *) printf 'Unknown profile: %s\n' "$profile" >&2; exit 2 ;;
esac
if [[ ${2:-} == --print ]]; then printf '%s\n' "$values"; exit; fi
[[ $# == 1 ]] || exit 2
serial=${ANDROID_SERIAL:?set ANDROID_SERIAL to the Quest device}
data=/sdcard/Android/data/com.lasrecomp.quest/files
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
printf '%s\n' "$values" > "$tmp"
# The old env is retained locally for diagnosis; no game data is copied or removed.
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
backup="$root/build/quest-profile-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$backup"
adb -s "$serial" pull "$data/env.txt" "$backup/previous-env.txt" >/dev/null 2>&1 || true
cp "$tmp" "$backup/applied-env.txt"
adb -s "$serial" shell am force-stop com.lasrecomp.quest
adb -s "$serial" push "$tmp" "$data/env.txt" >/dev/null
adb -s "$serial" shell am start -n com.lasrecomp.quest/android.app.NativeActivity
printf 'Applied %s; source and output sizes are different in the appsw preset.\n' "$profile"
