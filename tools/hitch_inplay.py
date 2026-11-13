# tools/hitch_inplay.py <run.log>...: late frames counted from the first frame in play ([inputrec] in play at presented
# frame N) to the end of the replay, by kind; one line per log
import sys, re, collections
for p in sys.argv[1:]:
    start = end = None; kinds = collections.Counter(); n = 0; worst = 0
    for l in open(p, errors="replace"):
        m = re.search(r"in play at presented frame (\d+)", l)
        if m: start = int(m.group(1)); continue
        m = re.search(r"replay over at frame (\d+)", l)
        if m and start is not None: end = start + int(m.group(1)); continue
        m = re.search(r"\[hitch\] frame (\d+) (\d+) ms: exec ([0-9.]+) \(gpu waits ([0-9.]+)\).*rest ([0-9.]+) \| shaders (\d+) .*pipelines (\d+) .*files (\d+) KB", l)
        if not m or start is None: continue
        f, ms, e, g, r, sh, pi, fi = m.groups(); f = int(f)
        if f < start or (end and f > end): continue
        n += 1; worst = max(worst, int(ms))
        k = "load" if int(fi) > 200 or int(sh) > 0 or int(pi) > 3 else "gpu" if float(g) > 4 else "encode" if float(e) > 10 else "cpu"
        kinds[k] += 1
    print(f"{p.split('/')[-2]:22s} in play: {n:4d} late frames (worst {worst} ms)  " + "  ".join(f"{k} {v}" for k, v in kinds.most_common()))
