# Task 1 Plan: Evaluate Lookup and Insertion Performance

## Goal
Compare lookup and insertion throughput of **B+Tree**, **Dynamic PGM**, and **LIPP** across three datasets, and explain the observed differences.

**Milestone 1 — Due 3/29 — 10% of points**

---

## Step 1: Run the Benchmark Pipeline

Execute `scripts/run_all.sh`, which performs these sub-steps:

1. **Download datasets** (`download_dataset.sh`)  
   - `fb_100M_public_uint64` (Facebook)
   - `books_100M_public_uint64` (Books)
   - `osmc_100M_public_uint64` (OSMC)

2. **Create minimal CMakeLists.txt** (`create_minimal_cmake.sh`)  
   Configures the build to include only the three required indexes.

3. **Generate workloads** (`generate_workloads.sh`)  
   For each dataset, generates 4 workloads (2M operations each):
   | Workload | Description |
   |---|---|
   | Lookup-only | 0% insert, 50% negative lookup |
   | Insert+Lookup (sequential) | 50% insert, 50% negative lookup (insert first, then lookup) |
   | Mixed 90% insert | 90% insert + 10% lookup, interleaved |
   | Mixed 10% insert | 10% insert + 90% lookup, interleaved |

4. **Build benchmark** (`build_benchmark.sh`)  
   Compiles the C++ benchmark binary.

5. **Run benchmarks** (`run_benchmarks.sh`)  
   Runs all 3 indexes × 3 datasets × 4 workloads, each repeated 3 times (`-r 3`).  
   Results are written to `results/*.csv`.

**Important:** Must be submitted as a Slurm job (`sbatch`), not run on the login node. Expected runtime ~30 min.

---

## Step 2: Analyze Results

Run `python scripts/analysis.py` to:
- Parse all CSV files in `results/`
- For B+Tree and DynamicPGM, select the hyperparameter configuration with the **highest average throughput** (averaged across 3 runs)
- Generate bar plots saved to `benchmark_results.png`
- Save summary tables to `analysis_results/`

---

## Step 3: Numbers to Report

For each dataset (FB, Books, OSMC), report the following metrics (best hyperparameter config, averaged over 3 runs):

### Table 1: Lookup-Only Throughput (Mops/s)
| Index | FB | Books | OSMC |
|---|---|---|---|
| B+Tree | _fill_ | _fill_ | _fill_ |
| DynamicPGM | _fill_ | _fill_ | _fill_ |
| LIPP | _fill_ | _fill_ | _fill_ |

### Table 2: Insert+Lookup — Insert Throughput (Mops/s)
| Index | FB | Books | OSMC |
|---|---|---|---|
| B+Tree | _fill_ | _fill_ | _fill_ |
| DynamicPGM | _fill_ | _fill_ | _fill_ |
| LIPP | _fill_ | _fill_ | _fill_ |

### Table 3: Insert+Lookup — Lookup Throughput After Insert (Mops/s)
| Index | FB | Books | OSMC |
|---|---|---|---|
| B+Tree | _fill_ | _fill_ | _fill_ |
| DynamicPGM | _fill_ | _fill_ | _fill_ |
| LIPP | _fill_ | _fill_ | _fill_ |

### Table 4: Mixed Workload — 10% Insert Throughput (Mops/s)
| Index | FB | Books | OSMC |
|---|---|---|---|
| B+Tree | _fill_ | _fill_ | _fill_ |
| DynamicPGM | _fill_ | _fill_ | _fill_ |
| LIPP | _fill_ | _fill_ | _fill_ |

### Table 5: Mixed Workload — 90% Insert Throughput (Mops/s)
| Index | FB | Books | OSMC |
|---|---|---|---|
| B+Tree | _fill_ | _fill_ | _fill_ |
| DynamicPGM | _fill_ | _fill_ | _fill_ |
| LIPP | _fill_ | _fill_ | _fill_ |

---

## Step 4: Writeup — Explain Observed Differences

The writeup should address **why** the throughput differs among the three indexes. Key points to cover:

### Lookup Performance
- **LIPP** should have the best lookup throughput because its learned model gives exact (precise) positions — no binary search needed within a node; lookups cost O(tree height) with no correction step.
- **DynamicPGM** predicts approximate positions using piecewise linear models, then does a local binary search within a 2ε window at each level. This extra correction cost slows lookups relative to LIPP.
- **B+Tree** uses standard multi-level tree traversal with comparisons at each internal node. Pointer chasing and cache misses make it slower than learned indexes for read-heavy workloads on sorted data.

### Insert Performance
- **DynamicPGM** uses a logarithmic buffering strategy (append to small buffers, merge when full) that gives efficient amortized insertion — especially for sequential/append workloads.
- **LIPP** handles insertion by model-guided traversal and conflict resolution (creating child nodes when collisions occur). This is efficient for moderate insert loads but may degrade with heavy inserts due to tree depth growth from frequent conflicts.
- **B+Tree** inserts into leaf nodes with potential splits propagating upward. Reasonable insert performance but incurs overhead from node splits and rebalancing.

### Mixed Workloads
- With 90% lookups / 10% inserts, LIPP's superior lookup performance should dominate.
- With 90% inserts / 10% lookups, DynamicPGM's efficient buffered insertion should give it an advantage.
- Actual results may vary by dataset due to different key distributions (FB keys are dense, Books keys are clustered, OSMC keys are geospatial).

### Dataset-Specific Observations
- Different key distributions affect how well each index's model fits the data, impacting both build time and query accuracy.
- Note any dataset where one index particularly excels or struggles and relate it to the data distribution.

---

## Step 5: Generate Final Deliverable

1. Include the bar plots from `benchmark_results.png` (4 subplots covering all workloads).
2. Include the filled-in tables above.
3. Include the explanation writeup (Step 4).
4. Optionally include the selected hyperparameter configurations for B+Tree and DynamicPGM.

---

## Checklist
- [ ] Run `scripts/run_all.sh` via `sbatch` on Adroit/Della
- [ ] Verify `results/*.csv` files are populated
- [ ] Run `python scripts/analysis.py` to generate plots and summaries
- [ ] Extract numbers for all 5 tables
- [ ] Write explanation of performance differences
- [ ] Compile final deliverable
