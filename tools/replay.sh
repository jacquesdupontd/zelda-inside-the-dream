#!/bin/bash
# tools/replay.sh <recording dir> <name> [VAR=value ...]: replays a recorded play session (runtime/hle/inputrec.c)
# full screen in front, from a copy of its save and with its own settings; the run goes to build/replays/runs/<name>/
# (run.log). Options are passed as environment for A/B runs, e.g. LARECOMP_ASYNC_SUBMIT=0.
#   LARECOMP_INPUTREC_SHOTS=<frame>,... : window shots at those frames in play (into the recording's folder)
# Ends 20 s after the recording does. Then: the late frames by kind (tools/hitch_kinds.py when present).
# Never while Guillaume's own game runs: two instances share the GPU and both measure wrong.
cd "$(dirname "$0")/.." || exit 1
R=$1; N=$2; shift 2
[ -f "$R/inputs.bin" ] || { echo "no $R/inputs.bin"; exit 1; }
if pgrep -qf "^./build/lasrecomp extracted"; then echo "his game is running: not replaying"; exit 1; fi
OUT=build/replays/runs/$N; rm -rf "$OUT"; mkdir -p "$OUT"
rm -rf /tmp/replay_save && cp -R "$R/save" /tmp/replay_save
cp "$R/larecomp.ini" /tmp/replay_settings.ini 2>/dev/null || printf "fullscreen = 1\nresolution = dynamic\n" > /tmp/replay_settings.ini
FRAMES=$(( $(stat -f %z "$R/inputs.bin") / 40 ))
T=$(( FRAMES / 60 + 50 ))
export LARECOMP_SAVE_DIR=/tmp/replay_save LARECOMP_SETTINGS=/tmp/replay_settings.ini LARECOMP_NO_AUDIO=1
# dynamic resolution: its factors per level (runtime/nvn/output_scale.c) start as in his game (build/dynres.txt; SEED=<file>
# or SEED=none) and are kept apart in /tmp/replay_dynres.txt, his own file left alone
if [ "${SEED:-build/dynres.txt}" = none ]; then rm -f /tmp/replay_dynres.txt; else cp "${SEED:-build/dynres.txt}" /tmp/replay_dynres.txt 2>/dev/null || rm -f /tmp/replay_dynres.txt; fi
export LARECOMP_DYNRES_FILE=/tmp/replay_dynres.txt
export LARECOMP_INPUT=200:A:6,320:L:8,320:R:8,480:A:6,640:A:6,800:A:6,950:DOWN:6,1010:A:6,1120:A:4,1180:A:4
export LARECOMP_INPUT_REPLAY="$R" LARECOMP_HITCH_PROF=1
for kv in "$@"; do export "$kv"; done
${BIN:-./build/lasrecomp} extracted --play > "$OUT/run.log" 2>&1 &
P=$!
( for i in 2 4 6 8 12; do sleep 1.5; osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $P) to true" >/dev/null 2>&1; done ) &
for i in $(seq 1 $T); do
  kill -0 $P 2>/dev/null || break
  grep -q "replay over" "$OUT/run.log" && { sleep 20; break; }
  sleep 1
done
kill $P 2>/dev/null; wait $P 2>/dev/null
echo "$N: $(grep "\[inputrec\]" "$OUT/run.log" | tr "\n" " ")"
