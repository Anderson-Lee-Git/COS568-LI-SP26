#include "benchmarks/benchmark_bloom_async_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "benchmarks/common.h"
#include "competitors/bloom_async_hybrid_pgm_lipp.h"

// ---------------------------------------------------------------------------
// Pareto / explicit search-class path
// Sweeps the Bloom-specific hyperparameters proposed for Milestone 3.
// Variants are: search_method, pgm_error, flush_threshold_per_mille, bits/key.
// ---------------------------------------------------------------------------
template <typename Searcher>
void benchmark_64_bloom_async_hybrid_pgm_lipp(
    tli::Benchmark<uint64_t>& benchmark,
    bool pareto,
    const std::vector<int>& params) {
  if (!pareto) {
    util::fail(
        "BloomAsyncHybridPGMLipp's hyperparameters cannot be set via --params");
  }

  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64,  2,  8>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64,  2, 10>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64,  2, 12>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64,  5,  8>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64,  5, 10>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64,  5, 12>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64, 10,  8>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64, 10, 10>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64, 10, 12>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64, 20,  8>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64, 20, 10>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 64, 20, 12>>();

  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 128,  5,  8>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 128,  5, 10>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 128, 10,  8>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 128, 10, 10>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 128, 20,  8>>();
  benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, Searcher, 128, 20, 10>>();
}

// ---------------------------------------------------------------------------
// File-based dispatch path (used when --pareto is NOT passed)
// Includes the configurations most likely to matter for the final workloads:
// lookup-heavy favors rare/no flush with Bloom fast path; insert-heavy keeps the
// async threshold sweep around the previously strongest region.
// ---------------------------------------------------------------------------
template <int record>
void benchmark_64_bloom_async_hybrid_pgm_lipp(
    tli::Benchmark<uint64_t>& benchmark,
    const std::string& filename) {
  if (filename.find("fb_100M") != std::string::npos ||
      filename.find("books_100M") != std::string::npos ||
      filename.find("osmc_100M") != std::string::npos) {
    if (filename.find("0.900000i") != std::string::npos) {
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128,  5,  8>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 10,  8>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 10, 10>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 20,  8>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 20, 10>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>,  64, 10,  8>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>,  64, 20,  8>>();
    } else if (filename.find("0.100000i") != std::string::npos) {
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>,  64,  2,  8>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>,  64,  5,  8>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>,  64,  5, 10>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>,  64, 10,  8>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>,  64, 10, 10>>();
      benchmark.template Run<BloomAsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>,  64, 20,  8>>();
    }
  }
}

INSTANTIATE_TEMPLATES_MULTITHREAD(
    benchmark_64_bloom_async_hybrid_pgm_lipp, uint64_t);
