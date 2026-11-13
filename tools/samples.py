# tools/samples.py <samples.txt> <unstripped .so or binary>...: the stacks runtime/sampler.c wrote (LARECOMP_SAMPLE),
# symbolized (llvm-symbolizer: LLVM_SYMBOLIZER=<path>, else the NDK's or the PATH's) and summed per function: self
# (the interrupted PC) and inclusive (anywhere in the stack, once per sample). Libraries are matched by file name.
#   --top N (default 40)   --callers <function substring>: who calls it (by the frame above)
#   --children <function substring>: what it was running (the frame below it: itself when the sample is in its own code)
import os, subprocess, sys, collections, glob
args = sys.argv[1:]
top = 40; callers = None; children = None
if "--top" in args: i = args.index("--top"); top = int(args[i + 1]); del args[i:i + 2]
if "--callers" in args: i = args.index("--callers"); callers = args[i + 1]; del args[i:i + 2]
if "--children" in args: i = args.index("--children"); children = args[i + 1]; del args[i:i + 2]
path, libs = args[0], {os.path.basename(p): p for p in args[1:]}
sym = os.environ.get("LLVM_SYMBOLIZER") or next(iter(sorted(glob.glob(os.path.expanduser(
    "~/android-sdk/ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-symbolizer")))), "llvm-symbolizer")
stacks = []
for l in open(path):
    if l.startswith("#"): print(l.strip()); continue
    f = l.split()
    if f: stacks.append((int(f[0]), f[1:]))
need = collections.defaultdict(set)
for _, st in stacks:
    for e in st:
        m, _, off = e.rpartition("+")
        if m in libs: need[m].add(int(off, 16))
names = {}
for m, offs in need.items():
    offs = sorted(offs)
    # (return addresses point after the call: one byte back lands in the calling function)
    q = "\n".join("0x%x" % max(o - 1, 0) for o in offs)
    out = subprocess.run([sym, "--obj=" + libs[m], "--functions=linkage", "--no-inlines", "--output-style=GNU"], input=q,
                         capture_output=True, text=True).stdout.split("\n")
    fn = [out[k] for k in range(0, len(out) - 1, 2)]
    for o, n in zip(offs, fn): names[m + "+%x" % o] = n if n and n != "??" else m + "+%x" % o
def name(e):
    if e in names: return names[e]
    m = e.rpartition("+")[0]
    return m if m not in ("", "?") else e
total = sum(c for c, _ in stacks)
self_c, incl = collections.Counter(), collections.Counter()
by_caller = collections.Counter(); by_child = collections.Counter()
for c, st in stacks:
    fr = [name(e) for e in st]
    self_c[fr[0]] += c
    for n in set(fr): incl[n] += c
    if children:
        for k in range(len(fr) - 1, -1, -1):  # (outermost first: the call that contains the others)
            if children in fr[k]:
                j = k - 1
                while j >= 0 and fr[j] == fr[k]: j -= 1
                by_child[fr[j] if j >= 0 else "(self)"] += c
                break
    if callers:
        for k in range(len(fr) - 1):
            if callers in fr[k] and fr[k + 1] != fr[k]: by_caller[fr[k + 1]] += c; break
print("%d samples" % total)
print("\n-- self")
for n, c in self_c.most_common(top): print("%6.2f%%  %s" % (100.0 * c / total, n))
print("\n-- inclusive")
for n, c in incl.most_common(top): print("%6.2f%%  %s" % (100.0 * c / total, n))
if callers:
    print("\n-- callers of %s" % callers)
    for n, c in by_caller.most_common(top): print("%6.2f%%  %s" % (100.0 * c / total, n))
if children:
    print("\n-- running under %s" % children)
    for n, c in by_child.most_common(top): print("%6.2f%%  %s" % (100.0 * c / total, n))
