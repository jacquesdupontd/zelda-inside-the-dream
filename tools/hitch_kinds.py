# tools/hitch_kinds.py <run.log>: the [hitch] lines by kind (loading, GPU wait, long encoding, the game CPU) and level
import sys, re, collections
c = collections.Counter(); n = 0; sums = collections.defaultdict(lambda: [0.0, 0.0, 0.0, 0.0, 0])
for l in open(sys.argv[1], errors="replace"):
    m = re.search(r"frame (\d+) (\d+) ms: exec ([0-9.]+) \(gpu waits ([0-9.]+)\) drawable ([0-9.]+) rest ([0-9.]+) \| shaders (\d+) .*pipelines (\d+) .*textures (\d+) .*files (\d+) KB \| (\S+)", l)
    if not m: continue
    f, ms, e, g, d, r, sh, pi, tx, fi, lv = m.groups(); n += 1
    if int(fi) > 200 or int(sh) > 0 or int(pi) > 3: k = "loading (files, shaders)"
    elif float(g) > 4: k = "GPU wait"
    elif float(e) > 10: k = "encoding > 10 ms"
    else: k = "game CPU (rest)"
    c[(k, lv)] += 1
    s = sums[k]; s[0] += float(ms); s[1] += float(e); s[2] += float(g); s[3] += float(r); s[4] += 1
print(n, "late frames")
for k, s in sums.items(): print(f"  {k}: {s[4]} frames, avg {s[0]/s[4]:.0f} ms = exec {s[1]/s[4]:.1f}, gpu waits {s[2]/s[4]:.1f}, rest {s[3]/s[4]:.1f}")
for (k, lv), v in c.most_common(10): print(f"  {v:4d} {k} {lv}")
