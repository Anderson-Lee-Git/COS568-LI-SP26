# Task 2 — Milestone 2 Plan: Naive Hybrid DPGM + LIPP

**Due: April 12, 2026 | Points: 30% of Task 2**

---

## 1. Overview

The goal is to implement a **naive hybrid index** that combines Dynamic PGM (DPGM) and LIPP:

- **LIPP** holds the bulk-loaded (initial) dataset and handles the majority of lookups.
- **DPGM** absorbs all new insertions.
- When DPGM grows beyond a **size threshold** (e.g., 5% of total keys), its contents are **flushed into LIPP** one key at a time (naive, synchronous migration).
- Lookups first probe DPGM (small and fast to scan), then fall back to LIPP if not found.

For Milestone 2, a **naive synchronous flush** is sufficient — no async, no double-buffering required.

---

## 2. Files to Create

| File | Purpose |
|------|---------|
| `competitors/hybrid_pgm_lipp.h` | Core hybrid index class (holds DPGM + LIPP, flush logic, lookup/insert routing) |
| `benchmarks/benchmark_hybrid_pgm_lipp.h` | Header declaring the benchmark template function |
| `benchmarks/benchmark_hybrid_pgm_lipp.cc` | Instantiates and runs the hybrid benchmark |

These follow the same pattern as `competitors/dynamic_pgm_index.h` and `benchmarks/benchmark_dynamic_pgm.{h,cc}`.

---

## 3. Implementation Plan

### 3.1 `competitors/hybrid_pgm_lipp.h`

Model this after `DynamicPGM` (which extends `Competitor<KeyType, SearchClass>`) and `Lipp` (which extends `Base<KeyType>`). The hybrid should extend one of them or implement the same interface directly.

Key members:
```cpp
template <class KeyType, class SearchClass, size_t pgm_error, size_t flush_threshold_pct = 5>
class HybridPGMLipp : public Competitor<KeyType, SearchClass> {
private:
    DynamicPGMIndex<KeyType, uint64_t, ...> dpgm_;
    LIPP<KeyType, uint64_t>                 lipp_;
    size_t total_keys_;        // tracks total key count for threshold calculation
    size_t dpgm_key_count_;    // tracks how many keys are in DPGM
};
```

**`Build(data, num_threads)`**
- Bulk-load all initial data into LIPP via `lipp_.bulk_load(...)`.
- Initialize `total_keys_ = data.size()`, `dpgm_key_count_ = 0`.

**`Insert(kv, thread_id)`**
- Insert into DPGM: `dpgm_.insert(kv.key, kv.value)`.
- Increment `dpgm_key_count_` and `total_keys_`.
- Check threshold: if `dpgm_key_count_ >= flush_threshold_pct / 100.0 * total_keys_`, call `Flush()`.

**`Flush()`**
- Iterate over all DPGM entries and insert each into LIPP individually.
- Clear DPGM (reconstruct empty instance).
- Reset `dpgm_key_count_ = 0`.

**`EqualityLookup(key, thread_id)`**
- Search DPGM first: `dpgm_.find(key)`.
- If not found (`it == dpgm_.end()`), search LIPP: `lipp_.find(key, value)`.
- Return `util::OVERFLOW` / `util::NOT_FOUND` on miss.

**`size()`**
- Return `dpgm_.size_in_bytes() + lipp_.index_size()`.

**`name()`** — return `"HybridPGMLipp"`.

**`applicable(...)`** — same as LIPP: `unique && !multithread`.

**`variants()`** — return `{SearchClass::name(), std::to_string(pgm_error), std::to_string(flush_threshold_pct)}`.

---

### 3.2 `benchmarks/benchmark_hybrid_pgm_lipp.h`

```cpp
#pragma once
#include "benchmark.h"

template <typename Searcher>
void benchmark_64_hybrid_pgm_lipp(tli::Benchmark<uint64_t>& benchmark,
                                   bool pareto, const std::vector<int>& params);
```

### 3.3 `benchmarks/benchmark_hybrid_pgm_lipp.cc`

- Include `benchmark_hybrid_pgm_lipp.h`, `benchmark.h`, `common.h`, and `competitors/hybrid_pgm_lipp.h`.
- In the pareto path, sweep over several flush thresholds (e.g., 1%, 5%, 10%, 20%) and a few `pgm_error` values to find best-performing hyperparameter.
- Instantiate using `INSTANTIATE_TEMPLATES_MULTITHREAD(benchmark_64_hybrid_pgm_lipp, uint64_t)`.

---

## 4. Workloads to Generate and Run

The README specifies two mixed workload types. Generate them for the **Facebook dataset only** (Milestone 2 requirement):

| Workload | Insert Ratio | Lookup Ratio | Workload Flag |
|----------|-------------|--------------|---------------|
| Lookup-heavy mixed | 10% insert | 90% lookup | `--insert-ratio 0.1 --negative-lookup-ratio 0.5 mix` |
| Insert-heavy mixed | 90% insert | 10% lookup | `--insert-ratio 0.9 --negative-lookup-ratio 0.5 mix` |

These correspond to workload files under `./data/` (same naming as `scripts/generate_workloads.sh`):

- **10% insert (mixed):** `fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix` (and companion `*_bulkload` if present)
- **90% insert (mixed):** `fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix` (and companion `*_bulkload` if present)

### Implemented tooling (in-repo)

| Script | Role |
|--------|------|
| `scripts/download_dataset.sh` | Downloads datasets into `./data` (run once). |
| `scripts/generate_workloads.sh` | Builds `generate` and creates workloads (includes both mixed workloads for `fb_100M_public_uint64`). |
| `scripts/build_benchmark.sh` | Configures CMake in `./build` and compiles the `benchmark` binary (includes `benchmark_hybrid_pgm_lipp.cc` via `CMakeLists.txt`). |
| `scripts/run_hybrid_benchmark.sh` | Runs **DynamicPGM**, **LIPP**, and **HybridPGMLipp** on the two Facebook mixed workloads with **`-r 3`**, writes CSVs under `./results/`, then prepends the mixed-workload CSV header. |
| `scripts/analysis_hybrid.py` | Reads those CSVs, picks the **best hyperparameter row** per index (max mean mixed throughput), and writes plots under `./analysis_results/`. |

**Cluster note (README):** do not run long benchmarks on login or vis nodes; submit jobs with `sbatch` so you get exclusive CPUs and stable numbers. The commands below assume an interactive compute session or a job that `cd`s to the repo root.

---

## 10. Scripts and commands to run (end-to-end)

Run everything from the **repository root** (the directory that contains `CMakeLists.txt` and `data/`).

### 10.1 One-time setup

```bash
# Optional: Python for plotting (README suggests Anaconda on Adroit)
# module load anaconda3/2023.3

bash scripts/download_dataset.sh
bash scripts/generate_workloads.sh
bash scripts/build_benchmark.sh
```

- `generate_workloads.sh` also compiles the project in `build/`; if you only need workloads, you can still run `build_benchmark.sh` afterward to ensure a Release build.

### 10.2 Milestone 2 benchmark run (Facebook, both mixed workloads, 3 repeats)

```bash
bash scripts/run_hybrid_benchmark.sh
```

This invokes `build/benchmark` six times per workload file (three indexes × two workloads), each with `--through --csv -r 3`. Result tables are written next to other benchmarks as:

- `results/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv`
- `results/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv`

### 10.3 Plots for the report (throughput and index size)

```bash
python scripts/analysis_hybrid.py
```

Outputs (by default):

- `analysis_results/hybrid_benchmark_results.png` — combined figure (throughput + size, both workloads)
- `analysis_results/hybrid_10_pct_insert_90_pct_lookup.png` — per-workload pair of bars
- `analysis_results/hybrid_90_pct_insert_10_pct_lookup.png` — per-workload pair of bars

The script labels bars as **DPGM**, **LIPP**, and **Hybrid**. For **DynamicPGM** and **HybridPGMLipp**, it keeps the CSV row with the **highest average** of `mixed_throughput_mops{1,2,3}` (same idea as Task 1: best hyperparameter line).

### 10.4 Manual `benchmark` invocations (optional)

Same binary as Task 1; use `--only` to select one index. **Mixed workloads** require `--through` (and usually `--csv` for tables).

**Lookup-heavy mixed (10% insert):**

```bash
build/benchmark \
  ./data/fb_100M_public_uint64 \
  ./data/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix \
  --through --csv --only HybridPGMLipp -r 3
```

**Insert-heavy mixed (90% insert):**

```bash
build/benchmark \
  ./data/fb_100M_public_uint64 \
  ./data/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix \
  --through --csv --only DynamicPGM -r 3
```

Replace `DynamicPGM` with `LIPP` or `HybridPGMLipp` as needed. Default search is `--search binary` (see `benchmark --help`).

**Correctness smoke test (slower):** add `--verify` to any of the above command lines.

**Broader hybrid hyperparameter sweep (optional):** uses the pareto path in `benchmarks/benchmark_hybrid_pgm_lipp.cc`:

```bash
build/benchmark \
  ./data/fb_100M_public_uint64 \
  ./data/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix \
  --through --csv --only HybridPGMLipp --pareto -r 3
```

### 10.5 Wiring already in the codebase

- `CMakeLists.txt` lists `benchmarks/benchmark_hybrid_pgm_lipp.cc` in `BENCH_SOURCES`.
- `benchmark.cc` dispatches `--only HybridPGMLipp` to `benchmark_64_hybrid_pgm_lipp` (same pattern as DynamicPGM / LIPP).

---

## 5. Hyperparameter Sweep

For DPGM: use the best-performing `pgm_error` found from Task 1 results on the Facebook dataset for the corresponding workload (lookup-heavy vs. insert-heavy).

For Hybrid: sweep over flush thresholds `{1%, 5%, 10%, 20%}` — report only the best in the final plots, but note the sweep results in the report.

---

## 6. Results Table and Plots

### 6.1 Results Table (include in report)

For each workload on the Facebook dataset:

| Index | Workload | Throughput (Mops/s) avg ± std | Index Size (MB) |
|-------|----------|-------------------------------|-----------------|
| DPGM (best ε) | 90% Lookup, 10% Insert | | |
| LIPP | 90% Lookup, 10% Insert | | |
| Hybrid (best threshold) | 90% Lookup, 10% Insert | | |
| DPGM (best ε) | 10% Lookup, 90% Insert | | |
| LIPP | 10% Lookup, 90% Insert | | |
| Hybrid (best threshold) | 10% Lookup, 90% Insert | | |

- Each throughput value is the average of 3 runs (use `-r 3`).

### 6.2 Bar Plots (4 total, as required)

| Plot | x-axis | y-axis | Bars |
|------|--------|--------|------|
| 1 | Index | Mixed Throughput (Mops/s) | DPGM, LIPP, Hybrid — 90% Lookup workload |
| 2 | Index | Mixed Throughput (Mops/s) | DPGM, LIPP, Hybrid — 90% Insert workload |
| 3 | Index | Index Size (MB) | DPGM, LIPP, Hybrid — 90% Lookup workload |
| 4 | Index | Index Size (MB) | DPGM, LIPP, Hybrid — 90% Insert workload |

---

## 7. Report Contents (1 page, concise)

The report should cover the following points:

1. **Hybrid Design Description**
   - Explain the role of DPGM (insertion buffer) and LIPP (lookup-optimized main store).
   - Describe the flush trigger: size-threshold-based synchronous flush.
   - Describe lookup routing: check DPGM first, fall back to LIPP.

2. **Implementation Notes**
   - Why naive flushing (individual inserts into LIPP) was used.
   - Any technical challenges encountered (e.g., LIPP does not support bulk loading after initial data is loaded, so individual inserts must be used during flush).

3. **Hyperparameter Selection**
   - Which `pgm_error` was chosen for DPGM and why (best average throughput from Task 1).
   - Which flush threshold was chosen for Hybrid and why (brief sweep result).

4. **Bar Plots** (the 4 plots listed in Section 6.2)

5. **Analysis and Discussion**
   - For the **lookup-heavy workload**: Does Hybrid approach LIPP performance? Why or why not? (Hypothesis: most lookups hit LIPP directly after early flushes; but flush cost may hurt during periods of heavy insertion.)
   - For the **insert-heavy workload**: Does Hybrid avoid the high insertion cost of LIPP? (Hypothesis: DPGM absorbs insertions cheaply, but frequent flushes may hurt.)
   - Index size comparison: Hybrid should be roughly equal to LIPP + small DPGM residual.
   - Identify the bottleneck of the naive approach: synchronous flush blocks all operations while iterating DPGM and inserting individually into LIPP — this motivates Milestone 3's async approach.

---

## 8. Milestone 2 Deliverables Checklist

See **Section 10** for the exact shell commands and scripts to produce CSVs and plots.

- [ ] `competitors/hybrid_pgm_lipp.h` implemented
- [ ] `benchmarks/benchmark_hybrid_pgm_lipp.h` implemented
- [ ] `benchmarks/benchmark_hybrid_pgm_lipp.cc` implemented
- [ ] CMakeLists updated to include new benchmark
- [ ] Workloads generated for `fb_100M` (10% insert, 90% insert mixed)
- [ ] Benchmarks run with `-r 3` for DPGM, LIPP, and Hybrid
- [ ] 4 bar plots generated (throughput + index size × 2 workloads)
- [ ] 1-page concise report written with plots and analysis
- [ ] Best DPGM hyperparameter selected and justified
- [ ] Best Hybrid flush threshold selected and justified

---

## 9. Key Constraints (from README)

- No keys may be lost or skipped during flush — correctness is enforced.
- No auxiliary data structures other than LIPP and DPGM are allowed.
- Each data point must be the average of at least 3 repeat runs.
- For Milestone 2: only the Facebook dataset is required.

## 10. Rerun all tasks
```
cd /scratch/gpfs/KOROLOVA/cl6486/COS568-LI-SP26

# Rebuild (needed once after pulling the fix)
bash scripts/build_benchmark.sh

# Optional: avoid appending to old CSVs / mixing runs
mkdir -p results/backup_milestone2
mv -f results/fb_100M_public_uint64_ops_2M_*_mix_results_table.csv results/backup_milestone2/ 2>/dev/null || true

# Re-run Milestone 2 benchmarks (3 repeats each)
bash scripts/run_hybrid_benchmark.sh

# Regenerate plots
python3 scripts/analysis_hybrid.py
```