#!/bin/bash
# qcompare.sh [variant.apk ...]: the headset versions side by side, prepared without the headset (21/10/2026), in
# ~/LASRecomp-quest/build/android/variants/ by default:
#   A0-reference-*  what was on the headset (first-person 2ff6902)
#   A1-quest-next-* + the texture fix + first person's scenery budget for the headset
#   B1-fusion-quest-* peiikai's beta_5 merged (its Vulkan work: lean usage / format lists, clears, render thread...)
# For each: installed (adb install -r keeps the game files), then qab.sh (forced 60 fps mode at the beach: the game
# thread's busy time, the fps) with a capture at frame 2600 (pulled: is the beach's sand the right colour? - it came out
# grey on the fusion's Vulkan path through MoltenVK), then qgpu.sh (GPU time per frame at full size).
# Never while he plays; check the other VR apps first (pidof). Results: ~/LASRecomp/build/quest/cmp_<variant>*.
Q=~/bin_q.sh
P=com.lasrecomp.quest
D=/sdcard/Android/data/$P/files
V=~/LASRecomp-quest/build/android/variants
OUT=~/LASRecomp/build/quest
mkdir -p $OUT
[ $# -gt 0 ] || set -- $V/A0-*.apk $V/A1-*.apk $V/B1-*.apk
echo "other apps: $($Q shell pidof $P) $($Q shell pidof com.lasrecomp.turniptest)"
for apk in "$@"; do
  n=cmp_$(basename "$apk" .apk | cut -d- -f1)
  echo "==== $n: $(basename "$apk")"
  $Q install -r "$apk" | tail -1
  $Q shell rm -rf $D/build/frames
  bash /tmp/qab.sh $n LARECOMP_SHOT_AT=2600
  $Q pull $D/build/frames/frame_02600.png $OUT/${n}_beach.png > /dev/null 2>&1 && echo "capture: $OUT/${n}_beach.png"
  bash /tmp/qgpu.sh ${n}_gpu | tail -3
done
