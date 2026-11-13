#!/usr/bin/env python3
"""Summarize existing Quest logs; keep raw logs and transition/capture events.
Usage: python3 tools/quest_metrics.py runtime.log [--after 45]
FPS windows require pending=0; pace windows also require the same level at both
ends. This is a scoped summary, not a claim about unobserved frames or scenes.
"""
import argparse
import json
import re
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log")
parser.add_argument("--after", type=float, default=45)
args = parser.parse_args()
rows, periods, batch = {}, {}, []
previous = None
cadence = []
events = []
scene_frames, scene_windows, verified = {}, [], {}
mv_history, display_history = {}, []
pattern = re.compile(r"\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms level (\S+).*pending (\d+).*\| gpu (\d+)")
for line in open(args.log):
    h = re.search(r"MV history: frame (\d+) previous (\d+)", line)
    if h:
        mv_history[int(h[1])] = int(h[2])
    h = re.search(r"display history: frame (\d+) previous (\d+) AppSW ([01])", line)
    if h:
        display_history.append(dict(frame=int(h[1]), previous_display=int(h[2]), appsw=bool(int(h[3]))))
    scene = re.search(r"\[scene\] frame (\d+) depth-frame (\d+) current ([01]) health (-?\d+)", line)
    if scene:
        scene_frames[int(scene[1])] = (int(scene[3]), int(scene[4]))
    p = re.search(r"\[pace\] period ([\d.]+)", line)
    if p:
        batch.append(float(p[1]))
    m = pattern.search(line)
    if m:
        t, frame, fps, maximum, level, pending, gpu = m.groups()
        t, frame = float(t), int(frame)
        if t >= args.after and int(pending) == 0 and previous and frame > previous[1]:
            rows.setdefault(level, []).append((float(fps), int(gpu)/(frame-previous[1]), int(maximum)))
            observed = [v for f, v in scene_frames.items() if previous[1] <= f < frame]
            expected = frame-previous[1]
            current = sum(v[0] for v in observed)
            alive = sum(v[1] > 0 for v in observed)
            qualified = len(observed) == expected and current == expected and alive == expected and previous[2] == level
            scene_windows.append(dict(t=t, frame=frame, level=level, fps=float(fps), expected_frames=expected,
                                      observed_frames=len(observed), current_scene_frames=current, alive_frames=alive,
                                      qualifies_3d_alive=qualified))
            if qualified:
                verified.setdefault(level, []).append(float(fps))
            if previous[0] >= args.after and previous[2] == level and previous[3] == 0:
                periods.setdefault(level, []).extend(batch)
        previous = (t, frame, level, int(pending))
        scene_frames = {f: v for f, v in scene_frames.items() if f >= frame}
        batch = []
    c = re.search(r"rendered images sampled ([\d.]+)/s, camera views ([\d.]+)/s, mismatched eye frame IDs (\d+)/(\d+)", line)
    if c:
        cadence.append(dict(images_per_s=float(c[1]), views_per_s=float(c[2]), mismatched=int(c[3]), checked=int(c[4])))
    if any(tag in line for tag in ("[hitch]", "[dynres]", "[resize]")):
        events.append(line.strip())

def percentile(values, q):
    s = sorted(values)
    index = (len(s)-1)*q
    lo = int(index)
    return s[lo] + (s[min(lo+1, len(s)-1)]-s[lo])*(index-lo)

out = {"log": args.log, "after_seconds": args.after, "levels": {}}
for level, samples in rows.items():
    item = dict(windows=len(samples), fps_median=statistics.median(x[0] for x in samples),
                fps_min=min(x[0] for x in samples), gpu_ms_median=statistics.median(x[1] for x in samples),
                max_frame_ms=max(x[2] for x in samples))
    values = periods.get(level, [])
    if values:
        item["pace_ms"] = dict(samples=len(values), p50=percentile(values,.5), p95=percentile(values,.95),
                               p99=percentile(values,.99), maximum=max(values),
                               over20=sum(x>20 for x in values), over33_3=sum(x>33.3 for x in values))
    out["levels"][level] = item
out["cadence"] = dict(mismatched=sum(c["mismatched"] for c in cadence), checked=sum(c["checked"] for c in cadence), windows=cadence)
out["events_all_phases_including_startup"] = events
out["scene_validation_notice"] = ("Unfiltered level FPS can include menus/death. Verified windows require per-frame "
                                  "current scene depth and positive health; absent diagnostics mean unknown, not valid.")
out["scene_windows"] = scene_windows
out["verified_3d_alive_levels"] = {level: dict(windows=len(v), fps_median=statistics.median(v), fps_min=min(v))
                                   for level, v in verified.items()}
for row in display_history:
    row["previous_mv"] = mv_history.get(row["frame"])
    row["matches"] = row["previous_mv"] == row["previous_display"] if row["previous_mv"] is not None else None
active = [row for row in display_history if row["appsw"]]
out["appsw_history"] = dict(observed=len(active), missing=sum(row["matches"] is None for row in active),
                           mismatched=sum(row["matches"] is False for row in active),
                           notice="Absent history diagnostics mean unknown. Matching predecessors do not prove vector math or visual quality.",
                           observations=display_history)
print(json.dumps(out, indent=2))
