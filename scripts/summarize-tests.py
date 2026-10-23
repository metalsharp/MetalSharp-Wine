#!/usr/bin/env python3
# Summarize Wine conformance runs (logs/winetest/<arch>/<module>/<unit>.log) into docs/TESTING.md.
# Usage: summarize-tests.py <root>
import glob, os, re, sys, datetime

root = sys.argv[1]
logs = os.path.join(root, "logs", "winetest")
out = []
out.append("# Conformance testing\n")
out.append(f"Generated {datetime.date.today()} by `scripts/summarize-tests.py` from the Wine 11.20 test suite, "
           "run with `scripts/run-winetest.sh` against the packaged `dist/` bundle. "
           "`aarch64` = native ARM64 test binaries; `x86_64` = x86_64 test binaries running through FEX (ARM64EC).\n")
out.append("A unit is *clean* when it exits 0. *Crash/timeout* means the unit did not print its summary "
           "(crash, debugger, or the 300 s limit).\n")

rx = re.compile(r"^[0-9a-f]+:(\S+): (\d+) tests executed \((\d+) marked as todo, \d+ as flaky, (\d+) failures?\), (\d+) skipped")
for arch in sorted(os.listdir(logs)):
    rows, tot = [], [0, 0, 0, 0, 0]
    for mod in sorted(os.listdir(os.path.join(logs, arch))):
        units = sorted(glob.glob(os.path.join(logs, arch, mod, "*.log")))
        clean = crash = checks = fails = 0
        failing = []
        for u in units:
            text = open(u, errors="ignore").read()
            m = None
            for line in text.splitlines():
                mm = rx.match(line.strip())
                if mm: m = mm
            name = os.path.basename(u)[:-4]
            if not m:
                crash += 1; failing.append(f"{name} (crash/timeout)")
                continue
            checks += int(m.group(2)); f = int(m.group(4)); fails += f
            if f: failing.append(f"{name} ({f})")
            else: clean += 1
        rows.append(f"| {mod} | {len(units)} | {clean} | {crash} | {checks:,} | {fails:,} | {', '.join(failing) or '-'} |")
        for i, v in enumerate([len(units), clean, crash, checks, fails]): tot[i] += v
    out.append(f"## {arch}\n")
    out.append("| module | units | clean | crash/timeout | checks | failures | failing units (failure count) |")
    out.append("|---|---|---|---|---|---|---|")
    out += rows
    rate = 100.0 * (tot[3] - tot[4]) / max(tot[3], 1)
    out.append(f"| **total** | **{tot[0]}** | **{tot[1]}** | **{tot[2]}** | **{tot[3]:,}** | **{tot[4]:,}** | {rate:.3f}% of checks pass |\n")

notes = os.path.join(root, "docs", "TESTING-notes.md")
if os.path.exists(notes): out.append(open(notes).read())
open(os.path.join(root, "docs", "TESTING.md"), "w").write("\n".join(out) + "\n")
print("\n".join(out))
