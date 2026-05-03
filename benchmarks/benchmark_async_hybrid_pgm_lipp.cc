#include "benchmarks/benchmark_async_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "benchmarks/common.h"
#include "competitors/async_hybrid_pgm_lipp.h"

// ---------------------------------------------------------------------------
// Pareto / explicit search-class path
// Sweeps the same pgm_error x flush_threshold_per_mille grid as the naive
// hybrid, so final plots can compare synchronous and asynchronous flushing under
// comparable hyperparameters.
// ---------------------------------------------------------------------------
template <typename Searcher>
void benchmark_64_async_hybrid_pgm_lipp(tli::Benchmark<uint64_t>& benchmark,
                                         bool pareto,
                                         const std::vector<int>& params) {
  if (!pareto) {
    util::fail("AsyncHybridPGMLipp's hyperparameters cannot be set via --params");
  }

  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 64,  1>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 64,  2>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 64,  5>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 64,  10>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 64,  20>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 128, 1>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 128, 2>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 128, 5>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 128, 10>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 128, 20>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 256, 2>>();
  benchmark.template Run<AsyncHybridPGMLipp<uint64_t, Searcher, 256, 5>>();
}

// ---------------------------------------------------------------------------
// File-based dispatch path (used when --pareto is NOT passed)
// Mirrors the representative configurations used by HybridPGMLipp.
// ---------------------------------------------------------------------------
template <int record>
void benchmark_64_async_hybrid_pgm_lipp(tli::Benchmark<uint64_t>& benchmark,
                                         const std::string& filename) {
  if (filename.find("fb_100M") != std::string::npos ||
      filename.find("books_100M") != std::string::npos ||
      filename.find("osmc_100M") != std::string::npos) {
    if (filename.find("0.900000i") != std::string::npos) {
      benchmark.template Run<AsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 5>>();
      benchmark.template Run<AsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 10>>();
      benchmark.template Run<AsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  5>>();
    } else if (filename.find("0.100000i") != std::string::npos) {
      benchmark.template Run<AsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 1>>();
      benchmark.template Run<AsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 2>>();
      benchmark.template Run<AsyncHybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64,  2>>();
    }
  }
}

INSTANTIATE_TEMPLATES_MULTITHREAD(benchmark_64_async_hybrid_pgm_lipp, uint64_t);
