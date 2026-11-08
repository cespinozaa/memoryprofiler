# compare.py: alternate two llama.cpp binaries (original vs patched) and compare them
# python3 compare.py RUNS BASE_BINARY PATCHED_BINARY [llama args...]

import csv
import hashlib
import os
import re
import statistics
import subprocess
import sys

runs, base, patched, args = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4:]
configs = {"base": base, "patched": patched}
metrics = ("wall_s", "peak_mb", "faults", "ms_per_token")
results = {name: {m: [] for m in metrics} for name in configs}
digests = set()


def one_run(binary):
    cmd = ["/usr/bin/time", "-f", "TIME %e %M %R", binary] + args
    p = subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL)
    if p.returncode != 0:
        sys.exit("run failed:\n" + p.stderr[-2000:])
    t = re.search(r"TIME ([0-9.]+) (\d+) (\d+)", p.stderr)
    ms = re.search(r"common_perf_print:\s+eval time =\s+[0-9.]+ ms /\s+\d+ runs\s+\(\s*([0-9.]+) ms per token",
                   p.stderr)
    digests.add(hashlib.md5(p.stdout.encode()).hexdigest())   # the generated text
    return {"wall_s": float(t.group(1)), "peak_mb": int(t.group(2)) / 1024,
            "faults": int(t.group(3)), "ms_per_token": float(ms.group(1)) if ms else float("nan")}


for i in range(runs):
    order = list(configs) if i % 2 == 0 else list(reversed(list(configs)))   # alternate who goes first
    for name in order:
        row = one_run(configs[name])
        for m in metrics:
            results[name][m].append(row[m])
        print("run %2d %-8s wall %6.2f s  peak %7.1f MB  faults %7d  %.1f ms/token" % (
            i + 1, name, row["wall_s"], row["peak_mb"], row["faults"], row["ms_per_token"]), flush=True)

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "compare_results.csv"), "w") as f:
    w = csv.writer(f)
    w.writerow(["config", "run"] + list(metrics))
    for name in configs:
        for i in range(runs):
            w.writerow([name, i + 1] + [results[name][m][i] for m in metrics])

print("\ngenerated text identical in every run: %s" % ("yes" if len(digests) == 1 else "NO, differs"))
print("%-13s %-12s %-12s %s" % ("metric", "base median", "patched med", "paired change, median [Q1 .. Q3]"))
for m in metrics:
    a, b = results["base"][m], results["patched"][m]
    changes = sorted(100 * (b[i] / a[i] - 1) for i in range(runs))
    q = statistics.quantiles(changes, n=4)
    print("%-13s %-12.2f %-12.2f %+.1f%%  [%+.1f%% .. %+.1f%%]" % (
        m, statistics.median(a), statistics.median(b), statistics.median(changes), q[0], q[2]))