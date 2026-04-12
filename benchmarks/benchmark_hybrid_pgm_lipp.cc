#include "benchmarks/benchmark_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "benchmarks/common.h"
#include "competitors/hybrid_pgm_lipp.h"

// ---------------------------------------------------------------------------
// Pareto / explicit search-class path
// Sweeps pgm_error x flush_threshold_per_mille (‰ of initial keys) so the caller
// can pick the best hyperparameters for the workload.
// ---------------------------------------------------------------------------
template <typename Searcher>
void benchmark_64_hybrid_pgm_lipp(tli::Benchmark<uint64_t>& benchmark,
                                   bool pareto,
                                   const std::vector<int>& params) {
  if (!pareto) {
    util::fail("HybridPGMLipp's hyperparameters cannot be set via --params");
  }

  // pgm_error in {64, 128, 256}; per-mille flush thresholds {1,2,5,10,20}
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 64,  1>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 64,  2>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 64,  5>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 64,  10>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 64,  20>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 128, 1>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 128, 2>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 128, 5>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 128, 10>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 128, 20>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 256, 2>>();
  benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 256, 5>>();
}

// ---------------------------------------------------------------------------
// File-based dispatch path (used when --pareto is NOT passed)
// Selects a small set of representative configurations per dataset / workload.
// ---------------------------------------------------------------------------
template <int record>
void benchmark_64_hybrid_pgm_lipp(tli::Benchmark<uint64_t>& benchmark,
                                   const std::string& filename) {
  // Facebook dataset — mixed workloads only (Milestone 2 focus)
  if (filename.find("fb_100M") != std::string::npos) {
    if (filename.find("0.900000i") != std::string::npos) {
      // Insert-heavy: moderate ‰ thresholds (500k–2M keys on 100M base).
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 5>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 10>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  5>>();
    } else if (filename.find("0.100000i") != std::string::npos) {
      // Lookup-heavy (~200k inserts): use low ‰ so DPGM actually flushes
      // (e.g. 2‰ of 100M = 200k keys) instead of staying full for all lookups.
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 1>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 2>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  2>>();
    }
  }

  // Books dataset
  if (filename.find("books_100M") != std::string::npos) {
    if (filename.find("0.900000i") != std::string::npos) {
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 5>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 10>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  5>>();
    } else if (filename.find("0.100000i") != std::string::npos) {
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 1>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 2>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  2>>();
    }
  }

  // OSMC dataset
  if (filename.find("osmc_100M") != std::string::npos) {
    if (filename.find("0.900000i") != std::string::npos) {
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 5>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 10>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  5>>();
    } else if (filename.find("0.100000i") != std::string::npos) {
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 1>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 2>>();
      benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  2>>();
    }
  }
}

INSTANTIATE_TEMPLATES_MULTITHREAD(benchmark_64_hybrid_pgm_lipp, uint64_t);
