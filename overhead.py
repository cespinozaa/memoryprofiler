# overhead.py: alternate plain and profiled runs of a program and compare them
# sudo python3 overhead.py RUNS INTERVAL_MS PROGRAM [ARGS...]

import csv
import math
import os
import re
import statistics
import subprocess
import sys
import time

runs, interval, cmd = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
here = os.path.dirname(os.path.abspath(__file__))
memprof = os.path.join(here, "memprof")
os.makedirs(os.path.join(here, "data", "raw"), exist_ok=True)
os.makedirs(os.path.join(here, "data", "results"), exist_ok=True)
csv_out = os.path.join(here, "data", "raw", "overhead_run.csv")
# both configs get the same allocator setting (memprof sets it for its target)
env = dict(os.environ, MALLOC_MMAP_THRESHOLD_="1048576")
configs = {"plain": cmd, "memprof": [memprof, csv_out, interval] + cmd}
# timings the target prints about itself; a metric a program does not print stays nan
fields = {"generate": r"Generate Time:\s+([0-9.]+)",
          "build": r"Build Time:\s+([0-9.]+)",
          "trial": r"Average Time:\s+([0-9.]+)",
          "load_ms": r"common_perf_print:\s+load time =\s+([0-9.]+) ms",
          "token_ms": r"common_perf_print:\s+eval time =\s+[0-9.]+ ms /\s+\d+ runs\s+\(\s*([0-9.]+) ms per token"}
keys = ["wall"] + list(fields)
results = {name: {k: [] for k in keys} for name in configs}


def one_run(argv):
    start = time.perf_counter()
    p = subprocess.run(argv, env=env, capture_output=True, text=True, stdin=subprocess.DEVNULL)
    wall = time.perf_counter() - start
    if p.returncode != 0:
        sys.exit("run failed:\n" + p.stderr[-2000:])
    text = p.stdout + p.stderr           # GAPBS prints to stdout, llama.cpp to stderr
    row = {"wall": wall}
    for key, pattern in fields.items():
        m = re.search(pattern, text)
        row[key] = float(m.group(1)) if m else float("nan")
    return row


for i in range(runs):
    order = list(configs) if i % 2 == 0 else list(reversed(list(configs)))   # alternate who goes first
    for name in order:
        row = one_run(configs[name])
        for key, value in row.items():
            results[name][key].append(value)
        shown = " ".join("%s %.3f" % (k, v) for k, v in row.items() if not math.isnan(v))
        print("run %2d %-7s %s" % (i + 1, name, shown), file=sys.stderr, flush=True)

with open(os.path.join(here, "data", "results", "overhead_results.csv"), "w") as f:
    w = csv.writer(f)
    w.writerow(["config", "run"] + keys)
    for name in configs:
        for i in range(runs):
            w.writerow([name, i + 1] + ["%.4f" % results[name][k][i] for k in keys])

print("\n%-9s %-20s %-20s %-22s %s" % ("metric", "plain median [min]", "memprof median [min]",
                                       "overhead median / min", "plain noise"))
for key in keys:
    a, b = results["plain"][key], results["memprof"][key]
    if any(math.isnan(v) for v in a + b):
        continue                           # the program does not print this one
    amed, bmed = statistics.median(a), statistics.median(b)
    print("%-9s %8.3f [%8.3f]    %8.3f [%8.3f]    %+6.1f%% / %+6.1f%%      sd %.0f%%" % (
        key, amed, min(a), bmed, min(b),
        100 * (bmed / amed - 1), 100 * (min(b) / min(a) - 1),
        100 * statistics.stdev(a) / statistics.mean(a)))