#pragma once
#include "benchmark.h"

// Pareto / explicit-params path: SearchClass is provided by the caller.
template <typename Searcher>
void benchmark_64_bloom_hybrid_pgm_lipp(
    tli::Benchmark<uint64_t>& benchmark,
    bool pareto,
    const std::vector<int>& params);

// File-based dispatch path: configuration is chosen based on the ops filename.
template <int record>
void benchmark_64_bloom_hybrid_pgm_lipp(
    tli::Benchmark<uint64_t>& benchmark,
    const std::string& filename);
