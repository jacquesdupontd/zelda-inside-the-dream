#!/usr/bin/env python3
# tools/hitchprof.py build/prof/hitch-<pid>.bin [--min MS] [--frames N] [--top K]
# Reads what runtime/hitchprof.c kept of the late frames (LARECOMP_HITCH_PROF=1) and names it: for the game and the
# runtime (lasgame.dylib, lasrecomp) with nm, for the system libraries by their name only. Prints, over all the late
# frames and then for the longest ones, where the frame thread was: by the innermost function (self) and by every
# function on the stack (inclusive: a game function and all it called). Nothing is filtered out: a sample the stack
# walk could not follow far shows as what it has.
import bisect, collections, os, struct, subprocess, sys

def text_vmaddr(path):
    out = subprocess.run(["otool", "-l", path], capture_output=True, text=True).stdout.split("\n")
    for i, l in enumerate(out):
        if "segname __TEXT" in l:
            for m in out[i:i + 4]:
                if "vmaddr" in m: return int(m.split()[1], 16)
    return 0

def symbols(path):
    out = subprocess.run(["nm", "-n", path], capture_output=True, text=True).stdout.split("\n")
    addrs, names = [], []
    for l in out:
        p = l.split()
        if len(p) >= 3 and p[1] in "tTsS":
            addrs.append(int(p[0], 16)); names.append(p[2].lstrip("_"))
    return addrs, names

def main():
    args = sys.argv[1:]
    path = args[0]
    mn = float(args[args.index("--min") + 1]) if "--min" in args else 0
    nfr = int(args[args.index("--frames") + 1]) if "--frames" in args else 6
    top = int(args[args.index("--top") + 1]) if "--top" in args else 14
    data = open(path, "rb").read()
    head_end = data.index(b"end\n") + 4
    images = []
    for l in data[:head_end].decode(errors="replace").split("\n"):
        if l.startswith("image "):
            _, a, name = l.split(" ", 2)
            images.append((int(a, 16), name))
    images.sort()
    starts = [a for a, _ in images]
    tables = {}
    for a, name in images:
        b = os.path.basename(name)
        if b in ("lasgame.dylib", "lasrecomp") and os.path.exists(name):
            slide = a - text_vmaddr(name)
            ad, nm = symbols(name)
            tables[name] = ([x + slide for x in ad], nm)
    def name_of(x):
        i = bisect.bisect_right(starts, x) - 1
        if i < 0: return "?"
        img = images[i][1]
        t = tables.get(img)
        if t:
            j = bisect.bisect_right(t[0], x) - 1
            return t[1][j] if j >= 0 else os.path.basename(img)
        return "[" + os.path.basename(img) + "]"
    frames = []
    o = head_end
    while o + 32 <= len(data):
        tag, frame, gap, n = struct.unpack_from("<IIII", data, o); o += 16
        if tag != 0x46524d46: break
        lv = data[o:o + 16].split(b"\0")[0].decode(errors="replace"); o += 16
        samples = []
        try:
            for _ in range(n):
                (k,) = struct.unpack_from("<I", data, o); o += 4
                samples.append(struct.unpack_from("<%dQ" % k, data, o)); o += 8 * k
        except struct.error:
            break  # (the game is still writing its last record)
        frames.append((frame, gap / 100.0, lv, samples))
    frames = [f for f in frames if f[1] >= mn]
    cache = {}
    def nm(x):
        if x not in cache: cache[x] = name_of(x)
        return cache[x]
    def report(fs, title):
        self_c, incl_c, tot = collections.Counter(), collections.Counter(), 0
        for _, _, _, ss in fs:
            for s in ss:
                tot += 1
                names = [nm(a) for a in s]
                if names: self_c[names[0]] += 1
                for x in set(names): incl_c[x] += 1
        print(f"== {title}: {tot} samples")
        print("   self:")
        for x, c in self_c.most_common(top): print(f"   {100 * c / max(tot, 1):5.1f}%  {x}")
        print("   on the stack:")
        for x, c in incl_c.most_common(top + 6): print(f"   {100 * c / max(tot, 1):5.1f}%  {x}")
    print(f"{len(frames)} late frames (>= {mn} ms) in {path}")
    report(frames, "all late frames")
    for f in sorted(frames, key=lambda f: -f[1])[:nfr]:
        report([f], f"frame {f[0]}, {f[1]:.0f} ms, {f[2]}")

main()
