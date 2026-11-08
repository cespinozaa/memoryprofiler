# heatmap.py: draw memprof's CSV as a heatmap, one strip per labeled region
# python3 heatmap.py run.csv out.png [--title TEXT] [--cmap NAME] [--mark SECONDS:LABEL ...]
# if the CSV has an accessed_pages column (memprof run with MEMPROF_READS=1), a third
# panel shows which pages were touched at all (read or written)

import argparse
import csv
import re

import matplotlib
matplotlib.use("Agg")                # draw to a file
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Patch
from matplotlib.ticker import PercentFormatter

parser = argparse.ArgumentParser()
parser.add_argument("src")
parser.add_argument("out")
parser.add_argument("--title", default=None)
parser.add_argument("--cmap", default="YlOrRd", help="matplotlib colormap; inferno gives the dark look")
parser.add_argument("--frames", type=int, default=3, help="how many stack frames to show per label")
parser.add_argument("--mark", action="append", default=[], metavar="SECONDS:LABEL",
                    help="draw a labeled vertical line at this time (can be repeated)")
args = parser.parse_args()


def short_one(label):
    # "pvector.h:30 (pvector<int>::pvector(unsigned long))" -> "pvector.h:30 pvector::pvector"
    loc, _, func = label.partition(" (")
    func = func.rstrip(")")
    prev = None
    while prev != func:              # drop template arguments, innermost first
        prev = func
        func = re.sub(r"<[^<>]*>", "", func)
    func = re.split(r"[<(]", func)[0]    # drop call arguments (or a label cut off mid-way)
    return loc + " " + func if func else loc


def short(label):
    # drop frames without source info (like _start, which shows up as "??:?")
    parts = [p for p in label.split(" <- ") if not p.startswith("??")][:args.frames]
    return "\n<- ".join(short_one(p) for p in parts) if parts else label


# the CSV names each region by an allocation-site id (S0, S1, ...); the labels file
# next to it says which source lines each id stands for (older CSVs have labels inline)
labels = {}
try:
    with open(args.src + ".labels") as f:
        for line in f:
            site, _, text = line.rstrip("\n").partition(",")
            labels[site] = text
except FileNotFoundError:
    pass

# one tuple per CSV line:
# (time_ms, region_start, label, bucket, written, accessed, present, region_pages)
rows = []
with open(args.src) as f:
    reader = csv.DictReader(f)
    has_acc = "accessed_pages" in reader.fieldnames
    for r in reader:
        rows.append((int(r["time_ms"]), int(r["region_start"], 16),
                     labels.get(r["label"], r["label"]),
                     int(r["bucket"]), int(r["written_pages"]),
                     int(r["accessed_pages"]) if has_acc else 0,
                     int(r["present_pages"]), int(r["region_pages"])))

times = sorted({r[0] for r in rows})
tindex = {t: i for i, t in enumerate(times)}

# a region is its start address plus its label, in the order they first appeared
size, npages, first = {}, {}, {}
for t, s, lab, b, _, _, _, pages in rows:
    k = (s, lab)
    size[k] = max(size.get(k, 0), b + 1)
    npages[k] = pages
    first[k] = min(first.get(k, t), t)
keys = sorted(size, key=lambda k: (first[k], k[0]))


def pages_per_row(k):
    # 256 pages per 1 MB row, except a shorter last row
    n = np.full(size[k], 256.0)
    n[-1] = npages[k] - 256 * (size[k] - 1)
    return n[:, None]


# the panels, left to right: (title, key into the data)
panels = [("Pages written in each interval", "w")]
if has_acc:
    panels.append(("Pages accessed (read or written)", "a"))
panels.append(("Pages in RAM", "p"))
ncol = len(panels)

# per region and panel: one row per 1 MB bucket, one column per sample (nan = not tracked yet)
data = {k: {code: np.full((size[k], len(times)), np.nan) for _, code in panels} for k in keys}
for t, s, lab, b, w, a, p, _ in rows:
    d = data[(s, lab)]
    d["w"][b, tindex[t]] = w
    d["p"][b, tindex[t]] = p
    if has_acc:
        d["a"][b, tindex[t]] = a

plt.rcParams.update({"font.size": 10, "axes.titlesize": 11})
cmap = plt.get_cmap(args.cmap).copy()
cmap.set_bad("0.85")                 # light gray where a region did not exist yet
marks = [(float(m.partition(":")[0]), m.partition(":")[2]) for m in args.mark]

# Sizes are set in inches. Each strip is as tall as its label needs (so labels never
# overlap) or as tall as its region's size suggests, whichever is more.
n = len(keys)
LINE_IN = 0.16                       # room for one line of label text
gap_in = 0.25                        # space between strips
top_in = 1.4 if marks else 1.1       # room above the strips: title (and phase labels)
bottom_in = 1.1                      # room below: time axis and legend
strip_in = [max(0.35 + 0.1 * size[k] ** 0.5, LINE_IN * (short(k[1]).count("\n") + 2) + 0.1)
            for k in keys]
height = sum(strip_in) + gap_in * (n - 1) + top_in + bottom_in
fig, axes = plt.subplots(n, ncol, figsize=(14 if ncol == 2 else 19, height), sharex=True,
                         squeeze=False,
                         gridspec_kw={"height_ratios": strip_in,
                                      "hspace": gap_in / (sum(strip_in) / n),
                                      "top": 1 - top_in / height, "bottom": bottom_in / height})
tspan = [times[0] / 1000, times[-1] / 1000]
for row, k in enumerate(keys):
    for col, (_, code) in enumerate(panels):
        ax = axes[row][col]
        im = ax.imshow(np.ma.masked_invalid(data[k][code] / pages_per_row(k)), aspect="auto",
                       extent=tspan + [size[k], 0], cmap=cmap, vmin=0, vmax=1)
        ax.set_yticks([])
        for spine in ax.spines.values():
            spine.set_color("0.6")
        for t, _ in marks:
            ax.axvline(t, color="0.25", linestyle="--", linewidth=0.8)
    axes[row][0].set_ylabel("%s\n%.0f MB" % (short(k[1]), npages[k] * 4096 / 2**20),
                            rotation=0, ha="right", va="center", fontsize=8.5)

# panel titles; the phase labels sit just above the top strip
for col, (title, _) in enumerate(panels):
    axes[0][col].set_title(title, fontweight="bold", pad=22 if marks else 8)
    for t, text in marks:
        axes[0][col].annotate(text, xy=(t, 1), xycoords=("data", "axes fraction"),
                              xytext=(3, 3), textcoords="offset points", fontsize=8, color="0.25")
for col in range(ncol):
    axes[-1][col].set_xlabel("time since start (s)")

cbar = fig.colorbar(im, ax=axes.ravel().tolist(), pad=0.02)
cbar.set_label("share of the row's pages (1 MB per row)")
cbar.ax.yaxis.set_major_formatter(PercentFormatter(1.0))

# the gray note only matters if some region is missing for part of the run
if any(np.isnan(m).any() for d in data.values() for m in d.values()):
    fig.legend(handles=[Patch(facecolor="0.85", edgecolor="0.6",
                              label="gray = region not allocated yet, or already freed")],
               loc="lower center", bbox_to_anchor=(0.45, 0.1 / height), frameon=False, fontsize=9)
fig.suptitle(args.title or args.src, fontsize=14, fontweight="bold", y=1 - 0.15 / height)
fig.savefig(args.out, dpi=150, bbox_inches="tight")