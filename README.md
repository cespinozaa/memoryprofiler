# memprof: a memory profiler that shows which memory is written, read, and held

CSC 501 (Operating Systems) Fall 2026 final project

**Team:** Cynthia Espinoza-Arredondo (cespino2), solo project

## Overview

memprof shows, over time, which parts of a program's large allocations are written,
which are accessed (read or written), and which are only sitting in RAM, and which
source line allocated each one. An eBPF program records every `mmap` and `munmap` of
1 MB or more together with the user call stack. Every N ms a sampler reads each
tracked page's soft-dirty and present bits from `/proc/PID/pagemap` and then resets
the soft-dirty bits through `/proc/PID/clear_refs`, so a set bit means "written since
the last sample". Optional read tracking uses the kernel's idle-page bitmap. After the
program exits, `addr2line` turns the call stacks into file:line labels, and
`heatmap.py` draws one strip per allocation (time on x-axis, position in the allocation
on y-axis, 1 MB per row).

It was checked on two known-answer programs (one for writes tracking and one for reads tracking) and used on GAPBS PageRank and llama.cpp.
In llama.cpp it found a 128 MB KV-cache buffer that is zero-filled at startup, and so
fully resident, but barely used. A small patch to ggml lowers peak memory by 8.7 %
(1416 MB to 1293 MB) with identical output.

## Repository layout

| Path | Contents |
|---|---|
| `memprof.bpf.c`, `memprof.c`, `memprof.h` | The profiler (eBPF part, user-space part, shared header) |
| `heatmap.py` | Draws the heatmaps from memprof's CSV |
| `overhead.py`, `pairs.py`, `compare.py` | Experiment scripts (overhead, paired statistics, llama.cpp fix comparison) |
| `microbench.c`, `readbench.c` | Known-answer test programs |
| `llama-lazy-clear.patch`, `apply_lazy_clear.py` | The llama.cpp change |
| `build.sh`, `setup.sh` | Build helper, dependency setup |
| `sampler.c`, `mmap_trace.bpf.c`, `mmap_trace.c`, `mmap_trace.h` | Early prototypes, later merged into memprof |
| `data/raw/` | memprof CSVs behind the report figures (other runs there are not committed) |
| `data/results/` | Overhead and comparison results behind the report tables |
| `figures/` | PNG figures |

## Code

### Written for this project

| File | What it does |
|---|---|
| `memprof.bpf.c` | eBPF program. Tracepoints on `sys_enter_mmap`, `sys_exit_mmap`, `sys_enter_munmap`, filtered to the target process. Records address, length and a user stack id (`bpf_get_stackid` with `BPF_F_USER_STACK` and `BPF_F_REUSE_STACKID`) and sends events through a ring buffer. |
| `memprof.c` | User-space profiler. Forks and starts the target (with `MALLOC_MMAP_THRESHOLD_=1048576` so `malloc` of 1 MB or more uses `mmap`), loads the eBPF program, keeps the list of live regions, samples `pagemap` every N ms and resets soft-dirty bits, optionally samples the idle-page bitmap, writes the CSV, and after the target exits resolves all stack frames with one batched `addr2line` call. |
| `memprof.h` | Event structure shared by the two parts. |
| `heatmap.py` | Plots a memprof CSV: two panels (written, in RAM) or three (written, accessed, in RAM) when read tracking is on. |
| `overhead.py` | Runs a program alternately without and with memprof, N pairs, and records the program's own timers (GAPBS and llama.cpp output) and wall time in `data/results/overhead_results.csv`. |
| `pairs.py` | Paired per-run overhead from an `overhead.py` result: median and quartiles per metric. |
| `compare.py` | Runs the original and patched llama.cpp alternately, N pairs, and compares peak RSS, page faults, speed and output (checksum). |
| `microbench.c` | Allocates 1 GB and writes only the first 100 MB, repeatedly (known answer: 25,600 written pages per sample). With `init_all` it fills the whole 1 GB once first. |
| `readbench.c` | Three 128 MB arrays: one written, one only read (volatile reads), one untouched. Known answer for read tracking. |
| `apply_lazy_clear.py` | Script that made the llama.cpp edit. `llama-lazy-clear.patch` is the same change as a git diff, use the patch. |
| `build.sh` | `./build.sh NAME` compiles `NAME.bpf.c`, generates the libbpf skeleton, and compiles `NAME.c`. |
| `setup.sh` | Installs dependencies and builds GAPBS and llama.cpp. |
| `sampler.c` | First prototype: stand-alone soft-dirty sampler for a given PID. |
| `mmap_trace.bpf.c`, `mmap_trace.c`, `mmap_trace.h` | First prototype: stand-alone eBPF mmap tracer. |

### Changed

**llama.cpp**: One file changed, `ggml/src/ggml-backend.cpp` (30 lines added, 1 removed), saved as
`llama-lazy-clear.patch`. It adds `ggml_backend_cpu_buffer_clear_anon`: when a CPU
buffer of 1 MB or more is cleared to zero, whole pages inside it are released with
`madvise(MADV_DONTNEED)` instead of written, so they read as zero and use RAM only once
written. Partial pages at the ends are cleared with `memset`, and every other case
falls back to the original `memset`.

### Used without changes

- **GAPBS**: Source unchanged; built with `-g -fno-omit-frame-pointer` added (see `setup.sh`)
- **Linux kernel**: stock Ubuntu 6.8.0-54-generic. No kernel changes. `vmlinux.h` is
  generated from the running kernel's BTF and is not committed
- **libbpf** 1.3.0 and **bpftool** 7.4.0 (Ubuntu packages)
- **addr2line** from binutils 2.42
- **Model**: Llama 3.2 1B Instruct, Q4_K_M GGUF (see Dependencies)

## Dependencies

Run, from the repository root:

```bash
./setup.sh
```

It installs the packages below with apt, checks the kernel features, clones and builds
GAPBS and llama.cpp (original and patched) at the commits above, and checks the model
file. Versions used:

| Package | Version |
|---|---|
| Ubuntu | 24.04.2 LTS |
| Kernel | 6.8.0-54-generic |
| clang | 18.1.3 |
| gcc | 13.3.0 |
| cmake | 3.28.3 |
| libbpf-dev | 1.3.0 |
| bpftool (linux-tools-common, linux-tools-6.8.0-54-generic) | 7.4.0 |
| libelf-dev | 0.190 |
| zlib1g-dev | 1.3 |
| binutils | 2.42 |
| Python | 3.12.3 |
| python3-matplotlib | 3.6.3 |
| python3-numpy | 1.26.4 |

`setup.sh` installs `linux-tools-$(uname -r)`; on a different kernel version that
package name changes with it.

**Kernel features needed:** BTF (`/sys/kernel/btf/vmlinux`), soft-dirty bits
(`/proc/PID/pagemap`, `/proc/PID/clear_refs`), and for read tracking only, idle page
tracking (`/sys/kernel/mm/page_idle/bitmap`). The stock Ubuntu 24.04 kernel has all of
them. memprof needs root.

**Model (manual download):** `Llama-3.2-1B-Instruct-Q4_K_M.gguf` (770 MB) from the Hugging Face
repository <https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF>.
`setup.sh` downloads it to `~/models/` if it is not there. To download it by hand:

```bash
wget -P ~/models https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/Llama-3.2-1B-Instruct-Q4_K_M.gguf
```
SHA-256 of the file used:

```
6f85a640a97cf2bf5b8e764087b1e83da0fdb51d7c9fab7d0fece9385611df83
```

## System

| | |
|---|---|
| Machine | NC State VCL virtual machine (KVM, full virtualization), on a shared host |
| CPU | Intel Xeon Processor (Skylake, IBRS), 4 vCPUs (2 sockets x 2 cores, 1 thread per core) |
| Memory | 7.8 GiB RAM, 2 GiB swap |
| OS | Ubuntu 24.04.2 LTS |
| Kernel | 6.8.0-54-generic |

Run times vary due to the host being shared (standard deviation 4-16 % between identical
runs). So experiments use alternating pairs and medians.

## Build

From the repository root, after `./setup.sh`:

```bash
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
```

```bash
./build.sh memprof
```

```bash
gcc -g -O1 -fno-omit-frame-pointer microbench.c -o microbench
```

```bash
gcc -g -O1 -fno-omit-frame-pointer readbench.c -o readbench
```

The prototypes are optional: `./build.sh mmap_trace` and
`gcc -O2 -Wall sampler.c -o sampler`.

## Run

```
sudo ./memprof OUT.csv INTERVAL_MS PROGRAM [ARGS...]
sudo env MEMPROF_READS=1 ./memprof OUT.csv INTERVAL_MS PROGRAM [ARGS...]   # also track reads
python3 heatmap.py OUT.csv OUT.png [--title T] [--frames N] [--mark SEC:LABEL ...] [--cmap NAME]
```

Example (the microbenchmark, 500 ms samples):

```bash
sudo ./memprof data/raw/microbench_final.csv 500 ./microbench
```

```bash
python3 heatmap.py data/raw/microbench_final.csv figures/microbench_final.png --title "Microbenchmark: 1 GB allocated, first 100 MB written" --frames 1
```

**Output.** `OUT.csv` has one row per sample, region and 1 MB bucket:
`time_ms,region_start,label,bucket,written_pages,present_pages,region_pages`, plus
`accessed_pages` with read tracking. `label` is a call-site id (`S<n>`);
`OUT.csv.labels` maps each id to its resolved stack (up to 6 frames). On exit memprof
prints the number of samples and the average time per sample to stderr.

**More Info:**

- The target must be built with `-g -fno-omit-frame-pointer` for useful labels. Only
  frames inside the target's own binary are resolved.
- Only mappings of 1 MB or more are tracked.
- The first sample of each region is discarded (new memory looks written), so a region
  that lives less than two sample intervals does not appear.
- Labels are resolved after the target exits, for llama.cpp's large debug binary this
  takes about 4 s.
- llama.cpp needs `-st` and `< /dev/null`, or it waits for input.
- `--mark` times in the commands below belong to the committed runs. A new run has
  different phase times, so adjust or drop them.

## Figures and tables

Every figure can be redrawn from the committed CSVs in `data/raw/` without root: run
only its plot command. Collecting new data needs root, the built binaries and the
model. Run everything from the repository root.

### Figure: microbenchmark (`figures/microbench_final.png`)

Data `data/raw/microbench_final.csv`. Collect and plot: the two commands in Run above.

### Figure: readbench, read-tracking known answer (`figures/readbench.png`)

Data `data/raw/readbench.csv`. `20` is the argument used for the report figure.

```bash
sudo env MEMPROF_READS=1 ./memprof data/raw/readbench.csv 500 ./readbench 20
```

```bash
python3 heatmap.py data/raw/readbench.csv figures/readbench.png --title "readbench: written vs accessed (known answer)" --frames 1
```

### Figure: GAPBS PageRank, write tracking (`figures/gapbs22_write.png`)

Data `data/raw/gapbs22_write.csv`.

```bash
sudo ./memprof data/raw/gapbs22_write.csv 500 ~/gapbs/pr -g 22 -n 10
```

```bash
python3 heatmap.py data/raw/gapbs22_write.csv figures/gapbs22_write.png --title "GAPBS PageRank (scale 22, about 1.1 GB): which memory is written" --frames 2 --mark 5.2:"build graph" --mark 20.8:"PageRank trials"
```

### Figure: GAPBS PageRank, read tracking (`figures/gapbs22_reads.png`)

Data `data/raw/gapbs22_reads.csv`.

```bash
sudo env MEMPROF_READS=1 ./memprof data/raw/gapbs22_reads.csv 1000 ~/gapbs/pr -g 22 -n 10
```

```bash
python3 heatmap.py data/raw/gapbs22_reads.csv figures/gapbs22_reads.png --title "GAPBS PageRank (scale 22): written vs accessed" --frames 2 --mark 3.9:"build graph" --mark 14.5:"PageRank trials"
```

### Figures: llama.cpp before and after the fix (`figures/llama_base2.png`, `figures/llama_fixed.png`)

Data `data/raw/llama_base2.csv` (original) and `data/raw/llama_fixed.csv` (patched). The
generated text is in `data/raw/llama_base2.out` and `data/raw/llama_fixed.out`.

```bash
sudo ./memprof data/raw/llama_base2.csv 250 ~/llama-completion-base -m ~/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf -c 4096 -n 128 -t 3 --seed 1 --temp 0 --ignore-eos -p "Once upon a time" -st < /dev/null > data/raw/llama_base2.out
```

```bash
sudo ./memprof data/raw/llama_fixed.csv 250 ~/llama.cpp/build/bin/llama-completion -m ~/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf -c 4096 -n 128 -t 3 --seed 1 --temp 0 --ignore-eos -p "Once upon a time" -st < /dev/null > data/raw/llama_fixed.out
```

```bash
python3 heatmap.py data/raw/llama_base2.csv figures/llama_base2.png --title "llama.cpp, original: which memory is written" --frames 5
```

```bash
python3 heatmap.py data/raw/llama_fixed.csv figures/llama_fixed.png --title "llama.cpp with the KV-cache fix: which memory is written" --frames 5
```

```bash
diff data/raw/llama_base2.out data/raw/llama_fixed.out && echo SAME
```

### Figures: llama.cpp read tracking (`figures/llama_reads_base.png` original, `figures/llama_reads.png` patched)

Data `data/raw/llama_reads_base.csv` and `data/raw/llama_reads.csv`.

```bash
sudo env MEMPROF_READS=1 ./memprof data/raw/llama_reads_base.csv 1000 ~/llama-completion-base -m ~/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf -c 4096 -n 128 -t 3 --seed 1 --temp 0 --ignore-eos -p "Once upon a time" -st > data/raw/llama_reads_base.log 2>&1 < /dev/null
```

```bash
sudo env MEMPROF_READS=1 ./memprof data/raw/llama_reads.csv 1000 ~/llama.cpp/build/bin/llama-completion -m ~/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf -c 4096 -n 128 -t 3 --seed 1 --temp 0 --ignore-eos -p "Once upon a time" -st > data/raw/llama_reads.log 2>&1 < /dev/null
```

```bash
python3 heatmap.py data/raw/llama_reads_base.csv figures/llama_reads_base.png --title "llama.cpp, original: written vs accessed" --frames 2
```

```bash
python3 heatmap.py data/raw/llama_reads.csv figures/llama_reads.png --title "llama.cpp: written vs accessed" --frames 2
```

### Table: microbenchmark written pages per sample

From `data/raw/microbench_final.csv`; prints each sample's time (ms) and total written
pages. Expected: 25,600 (100 MB) per sample.

```bash
awk -F, 'NR>1 {w[$1]+=$5} END {for (t in w) print t, w[t]}' data/raw/microbench_final.csv | sort -n
```

### Table: write-tracking overhead

Results `data/results/gapbs22_{100,500,1000}ms.csv` (10 pairs) and
`data/results/llama_t3_{100,500,1000}ms.csv` (15 pairs). Collecting takes tens of
minutes (so a while) per program:

```bash
for ms in 100 500 1000; do sudo python3 overhead.py 10 $ms ~/gapbs/pr -g 22 -n 10 | tail -7; cp data/results/overhead_results.csv data/results/gapbs22_${ms}ms.csv; done
```

```bash
for ms in 100 500 1000; do sudo python3 overhead.py 15 $ms ~/llama.cpp/build/bin/llama-completion -m ~/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf -c 4096 -n 128 -t 3 --seed 1 --temp 0 --ignore-eos -p "Once upon a time" -st | tail -7; cp data/results/overhead_results.csv data/results/llama_t3_${ms}ms.csv; done
```

Summarize (paired median overhead and quartiles per metric):

```bash
for f in gapbs22_100ms gapbs22_500ms gapbs22_1000ms llama_t3_100ms llama_t3_500ms llama_t3_1000ms; do echo "== $f"; python3 pairs.py data/results/$f.csv; done
```

### Table: read-tracking overhead

Results `data/results/gapbs22_reads_{1000,2000}ms.csv` and
`data/results/llama_reads_{1000,2000}ms.csv` (10 pairs each).

```bash
for ms in 1000 2000; do sudo env MEMPROF_READS=1 python3 overhead.py 10 $ms ~/gapbs/pr -g 22 -n 10 | tail -7; cp data/results/overhead_results.csv data/results/gapbs22_reads_${ms}ms.csv; done
```

```bash
for ms in 1000 2000; do sudo env MEMPROF_READS=1 python3 overhead.py 10 $ms ~/llama.cpp/build/bin/llama-completion -m ~/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf -c 4096 -n 128 -t 3 --seed 1 --temp 0 --ignore-eos -p "Once upon a time" -st | tail -7; cp data/results/overhead_results.csv data/results/llama_reads_${ms}ms.csv; done
```

```bash
for f in gapbs22_reads_1000ms gapbs22_reads_2000ms llama_reads_1000ms llama_reads_2000ms; do echo "== $f"; python3 pairs.py data/results/$f.csv; done
```

### Table: llama.cpp fix (peak memory, page faults, speed, output)

Results `data/results/compare_results.csv` (10 alternating pairs, 4 threads). Does not
use memprof and does not need root.

```bash
python3 compare.py 10 ~/llama-completion-base ~/llama.cpp/build/bin/llama-completion -m ~/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf -c 4096 -n 128 -t 4 --seed 1 --temp 0 --ignore-eos -p "Once upon a time" -st
```

### Files not used in the report

Other PNGs in `figures/` and the `data/results/overhead_*ms*.csv` files are earlier
exploratory/experimental runs (GAPBS scale 20, older memprof builds). They are kept for the record
only.
