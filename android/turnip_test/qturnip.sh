#!/bin/bash
# qturnip.sh [driver.so]: the Turnip-under-OpenXR test on the headset (android/turnip_test, LASRecomp-quest worktree).
# Two runs of 30 s - the system driver first (the baseline: the test itself works), then Turnip - each with its log
# pulled and 5 s of the headset's view recorded at 12 s: ~/LASRecomp-quest/build/quest/turnip_<system|turnip>.{log,mp4}.
# What to read: "plain:" (which driver a plain Vulkan instance gets), "OpenXR:" (the device OpenXR uses), "swapchain",
# "RESULT"; in the videos, the left eye red and the right eye blue, pulsing. Also logcat's crash buffer after each run.
Q=~/bin_q.sh
P=com.lasrecomp.turniptest
D=/sdcard/Android/data/$P/files
DRV=${1:-$HOME/turnip-quest/xemu07b/vulkan.ad07xx.so}
OUT=~/LASRecomp-quest/build/quest
mkdir -p $OUT
echo "other VR apps running (they share the GPU): $($Q shell pidof com.lasrecomp.quest) $($Q shell 'ps -A -o NAME' | grep -i -E 'oculus.*game|vr\.' | head -3 | tr '\n' ' ')"
$Q install -r ~/LASRecomp-quest/build/android/turnip-test.apk | tail -1
$Q shell mkdir -p $D/turnip
$Q push "$DRV" $D/turnip/vulkan.ad07xx.so > /dev/null
$Q shell chmod -R a+rX $D/turnip  # (made by adb: owned by shell, closed to the app - Permission denied)
$Q shell am broadcast -a com.oculus.vrpowermanager.prox_close > /dev/null 2>&1
for mode in ${MODES:-system turnip}; do  # (MODES=turnip: that run only)
  if [ $mode = system ]; then $Q shell touch $D/use_system; else $Q shell rm -f $D/use_system; fi
  $Q shell rm -f $D/turnip_test.log $D/env.txt
  if [ -n "$TENV" ]; then printf "%s\n" $TENV > /tmp/tt_env.txt; $Q push /tmp/tt_env.txt $D/env.txt > /dev/null; $Q shell chmod a+r $D/env.txt; fi  # (TENV="TU_DEBUG=noubwc ...")
  $Q shell am force-stop $P  # (a fresh process: only the test app, never the game)
  $Q logcat -c
  $Q shell am start -n $P/android.app.NativeActivity > /dev/null
  sleep 12
  $Q shell screenrecord --time-limit 5 /sdcard/turnip_$mode.mp4
  sleep 16
  $Q pull $D/turnip_test.log $OUT/turnip_$mode.log > /dev/null 2>&1
  $Q pull /sdcard/turnip_$mode.mp4 $OUT/turnip_$mode.mp4 > /dev/null 2>&1
  $Q logcat -d -b crash > $OUT/turnip_${mode}_crash.txt
  $Q logcat -d -s xrturnip > $OUT/turnip_${mode}_logcat.txt
  echo "== $mode"
  grep -E "plain|OpenXR|swapchain|RESULT|adrenotools|Turnip|failed|xrCreate" $OUT/turnip_$mode.log 2>/dev/null || tail -20 $OUT/turnip_${mode}_logcat.txt
  [ -s $OUT/turnip_${mode}_crash.txt ] && { echo "-- crash buffer:"; head -20 $OUT/turnip_${mode}_crash.txt; }
done
