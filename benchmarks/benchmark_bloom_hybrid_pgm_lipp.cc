#include "benchmarks/benchmark_bloom_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "benchmarks/common.h"
#include "competitors/bloom_hybrid_pgm_lipp.h"

// ---------------------------------------------------------------------------
// Pareto / explicit search-class path
// Sweeps the lookup-heavy Bloom configurations from the Milestone 3 plan.
// Variants are: search_method, pgm_error, flush_threshold_per_mille,
// bits/key, hash_count.
// ---------------------------------------------------------------------------
template <typename Searcher>
void benchmark_64_bloom_hybrid_pgm_lipp(
    tli::Benchmark<uint64_t>& benchmark,
    bool pareto,
    const std::vector<int>& params) {
  if (!pareto) {
    util::fail(
        "BloomHybridPGMLipp's hyperparameters cannot be set via --params");
  }

  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 64,  3, 2, 1>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 64,  3, 2, 2>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 64,  3, 4, 1>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 64,  3, 4, 2>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 64,  3, 4, 3>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 64,  3, 8, 1>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 64,  5, 4, 1>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 64,  5, 4, 2>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 64,  5, 4, 3>>();

  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 128, 3, 4, 1>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 128, 3, 4, 2>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 128, 3, 8, 1>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 128, 5, 4, 1>>();
  benchmark.template Run<BloomHybridPGMLipp<uint64_t, Searcher, 128, 5, 4, 2>>();
}

// ---------------------------------------------------------------------------
// File-based dispatch path (used when --pareto is NOT passed)
// Prioritizes lookup-heavy mixed workloads, with a small insert-heavy sanity set.
// ---------------------------------------------------------------------------
template <int record>
void benchmark_64_bloom_hybrid_pgm_lipp(
    tli::Benchmark<uint64_t>& benchmark,
    const std::string& filename) {
  if (filename.find("fb_100M") != std::string::npos ||
      filename.find("books_100M") != std::string::npos ||
      filename.find("osmc_100M") != std::string::npos) {
    if (filename.find("0.100000i") != std::string::npos) {
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  3, 2, 1>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  3, 2, 2>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  3, 4, 1>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  3, 4, 2>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  3, 4, 3>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  3, 8, 1>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  5, 4, 1>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  5, 4, 2>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  5, 4, 3>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 3, 4, 1>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 3, 4, 2>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 3, 8, 1>>();
    } else if (filename.find("0.900000i") != std::string::npos) {
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 10, 4, 3>>();
      benchmark.template Run<BloomHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 20, 4, 3>>();
    }
  }
}

INSTANTIATE_TEMPLATES_MULTITHREAD(
    benchmark_64_bloom_hybrid_pgm_lipp, uint64_t);
