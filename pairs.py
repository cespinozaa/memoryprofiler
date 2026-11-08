# pairs.py: paired overhead from an overhead_results CSV: compare each memprof run with
# the plain run made right next to it, so slow drift on a shared VM cancels out
# python3 pairs.py data/results/llama_t3_100ms.csv

import csv
import math
import statistics
import sys

plain, prof = {}, {}
with open(sys.argv[1]) as f:
    reader = csv.DictReader(f)
    cols = [c for c in reader.fieldnames if c not in ("config", "run")]
    for r in reader:
        (plain if r["config"] == "plain" else prof)[int(r["run"])] = r

print("%-10s %-30s %s" % ("metric", "median overhead [Q1 .. Q3]", "pairs"))
for key in cols:
    vals = [(float(plain[i][key]), float(prof[i][key])) for i in plain]
    if any(math.isnan(a) or math.isnan(b) for a, b in vals):
        continue                           # this program does not print this metric
    ratios = sorted(100 * (b / a - 1) for a, b in vals)
    q = statistics.quantiles(ratios, n=4)  # [Q1, median, Q3]
    print("%-10s %+6.1f%%  [%+.1f%% .. %+.1f%%]        %d" % (
        key.replace("_s", ""), statistics.median(ratios), q[0], q[2], len(ratios)))