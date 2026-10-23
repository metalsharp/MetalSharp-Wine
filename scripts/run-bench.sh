#!/bin/zsh
# Reproducible performance measurement: native ARM64 vs x86_64 (ARM64EC) and i386 (WoW64) through FEX.
# Writes docs/PERFORMANCE.md. Usage: run-bench.sh [wine-tree]   (default: install/)
# WINE_APP=path/to/MetalSharp-Wine.app runs through the signed bundle instead, which is required for i386
# (low 4G mappings need the cross-architecture entitlement); without it the i386 column is skipped.
source ${0:A:h}/env.sh
W=${1:-$ROOT/install}
if [[ -n $WINE_APP ]]; then
  WINE=$WINE_APP/Contents/MacOS/wine WINESERVER=$WINE_APP/Contents/MacOS/wineserver
  ARCHS=(aarch64 x86_64 i686)
else
  WINE=$W/bin/wine WINESERVER=$W/bin/wineserver
  ARCHS=(aarch64 x86_64)
fi
export WINEPREFIX=$ROOT/prefixes/bench WINEDEBUG=-all
OUT=$ROOT/docs/PERFORMANCE.md
B=$ROOT/tests/bin
L=$ROOT/logs/bench; rm -rf $L; mkdir -p $L

[[ -d $WINEPREFIX ]] || { $WINE wineboot -i >/dev/null 2>&1; $WINESERVER -w; }

ms() { python3 -c 'import time; print(int(time.time()*1000))'; }
run() { $WINE "$@" 2>/dev/null | tr -d '\r'; }

# startup latency
for a in $ARCHS; do
  $WINESERVER -k 2>/dev/null; sleep 1
  t0=$(ms); run $B/hello-$a.exe >/dev/null; echo $(( $(ms) - t0 )) > $L/cold-$a
done
$WINESERVER -p 600 >/dev/null 2>&1
run $B/hello-aarch64.exe >/dev/null
for a in $ARCHS; do
  t0=$(ms); for i in 1 2 3 4 5; do run $B/hello-$a.exe >/dev/null; done; echo $(( ($(ms) - t0) / 5 )) > $L/warm-$a
done

# micro + real-world workloads (best of 3 for the C benchmark)
for a in $ARCHS; do
  for i in 1 2 3; do run $B/bench-$a.exe > $L/bench-$a-$i.log; done
  run $B/kuser-$a.exe > $L/kuser-$a.log
  run $B/threads-$a.exe > $L/threads-$a.log
  run $B/jitmt-$a.exe 1 > $L/jitmt1-$a.log
  run $B/jitmt-$a.exe 8 > $L/jitmt8-$a.log
done
typeset -A PY=(aarch64 py-arm64 x86_64 py-amd64 i686 py-win32)
for a in $ARCHS; do run $ROOT/tests/apps/$PY[$a]/python.exe $ROOT/tests/apps/pybench.py > $L/py-$a.log; done
$WINESERVER -k 2>/dev/null

python3 - $L $OUT $ARCHS <<'EOF'
import re, sys, glob, subprocess, datetime
L, OUT, archs = sys.argv[1], sys.argv[2], sys.argv[3:]
names = {"aarch64": "native ARM64", "x86_64": "x86_64 (FEX)", "i686": "i386 (FEX)"}
emu = [a for a in archs if a != "aarch64"]
def read(path):
    try: return open(path).read()
    except OSError: return ""
def bench(arch):
    best = {}
    for f in sorted(glob.glob(f"{L}/bench-{arch}-*.log")):
        for line in open(f):
            m = re.match(r"(\w+)\s+([\d.]+) ms\s+(.*)", line)
            if m:
                k, t, c = m.group(1), float(m.group(2)), m.group(3).strip()
                if k not in best or t < best[k][0]: best[k] = (t, c)
    return best
def grab(path, rx):
    m = re.search(rx, read(path)); return m.groups() if m else None
def py(path):
    r = {}
    for line in read(path).splitlines():
        m = re.match(r"(t_\w+|total)\s+([\d.]+) ms\s*(.*)", line)
        if m: r[m.group(1)] = (float(m.group(2)), m.group(3).strip())
    return r
def row(cells): return "| " + " | ".join(cells) + " |"
def head(first, cols): return row([first] + cols) + "\n" + row(["---"] * (len(cols) + 1))

cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
osv = subprocess.run(["sw_vers", "-productVersion"], capture_output=True, text=True).stdout.strip()
out = ["# Performance\n",
       f"Measured {datetime.date.today()} on {cpu}, macOS {osv}, with `scripts/run-bench.sh`. "
       "x86 code runs through FEX with software x86 memory-ordering emulation: no Rosetta, no hardware TSO. "
       "x86_64 uses ARM64EC; i386 uses WoW64 and needs the signed MetalSharp-Wine.app (4K pages, low 4G). "
       "Ratios are emulated time / native ARM64 time (lower is better).\n"]

out.append("## Startup\n\n" + head("", [names[a] for a in archs]))
out.append(row(["cold (wineserver + services start)"] + [read(f"{L}/cold-{a}").strip() + " ms" for a in archs]))
out.append(row(["warm (server running)"] + [read(f"{L}/warm-{a}").strip() + " ms" for a in archs]) + "\n")

res = {a: bench(a) for a in archs}
cols = [names[a] for a in archs] + [f"ratio {a}" for a in emu] + ["checksums match"]
out.append("## C workloads (best of 3)\n\n" + head("test", cols))
for k in res["aarch64"]:
    if not all(k in res[a] for a in emu): continue
    base = res["aarch64"][k]
    same = all(res[a][k][1] == base[1] for a in emu)
    out.append(row([k] + [f"{res[a][k][0]:.2f} ms" for a in archs] +
                   [f"{res[a][k][0] / base[0]:.2f}" for a in emu] + ["yes" if same else "NO"]))
out.append("")

out.append("## System behaviour\n\n" + head("test", [names[a] for a in archs]))
def sysrow(label, file, rx, fmt):
    vals = [grab(f"{L}/{file}-{a}.log", rx) for a in archs]
    if all(vals): out.append(row([label] + [fmt(v) for v in vals]))
sysrow("direct KUSER_SHARED_DATA read", "kuser", r"\(([\d.]+) ns/read\)", lambda v: f"{v[0]} ns")
sysrow("thread create + join", "threads", r"\(([\d.]+) us each\)", lambda v: f"{v[0]} us")
sysrow("2000-function JIT stress, 1 thread (cold / warm)", "jitmt1", r"cold ([\d.]+) ms, warm ([\d.]+)", lambda v: f"{v[0]} / {v[1]} ms")
sysrow("2000-function JIT stress, 8 threads (cold / warm)", "jitmt8", r"cold ([\d.]+) ms, warm ([\d.]+)", lambda v: f"{v[0]} / {v[1]} ms")
out.append("")

pys = {a: py(f"{L}/py-{a}.log") for a in archs}
cols = [names[a] for a in archs] + [f"ratio {a}" for a in emu] + ["results match"]
out.append("## CPython 3.14.9 (official python.org embeddable builds)\n\n" + head("test", cols))
for k in pys["aarch64"]:
    if not all(k in pys[a] for a in emu): continue
    base = pys["aarch64"][k]
    same = "-" if k == "total" else ("yes" if all(pys[a][k][1] == base[1] for a in emu) else "NO")
    out.append(row([k] + [f"{pys[a][k][0]:.1f} ms" for a in archs] +
                   [f"{pys[a][k][0] / max(base[0], 0.1):.2f}" for a in emu] + [same]))
open(OUT, "w").write("\n".join(out) + "\n")
print(open(OUT).read())
EOF
