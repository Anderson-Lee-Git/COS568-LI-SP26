#ifndef TLI_HYBRID_PGM_LIPP_H
#define TLI_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "../util.h"
#include "base.h"
#include "pgm_index_dynamic.hpp"
#include "./lipp/src/core/lipp.h"

// Hybrid index: LIPP holds bulk-loaded data and is the primary lookup store.
// DPGM acts as an insertion buffer. When DPGM holds flush_threshold_per_mille
// **per-mille** (‰) of the initial (bulk-loaded) key count, all DPGM entries are
// flushed individually into LIPP and DPGM is reset to an empty state.
//
// Rationale: on 100M keys, "integer percent" (÷100) makes the smallest threshold
// 1M keys, so a 2M-op mixed workload with ~200k inserts never flushes. Per-mille
// (÷1000) keeps the same template parameter name in CSV variants while allowing
// realistic thresholds (e.g. 2‰ → 200k keys on 100M).
//
// Lookup: probe DPGM first, fall back to LIPP on miss.
// Insert: always insert into DPGM; flush to LIPP when threshold is reached.

template <class KeyType, class SearchClass, size_t pgm_error = 64,
          size_t flush_threshold_per_mille = 5>
class HybridPGMLipp : public Competitor<KeyType, SearchClass> {
  using DPGMType = DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                                   PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

 public:
  HybridPGMLipp(const std::vector<int>& params) {}

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data, size_t num_threads) {
    std::vector<std::pair<KeyType, uint64_t>> loading_data;
    loading_data.reserve(data.size());
    for (const auto& itm : data) {
      loading_data.emplace_back(itm.key, itm.value);
    }

    uint64_t build_time = util::timing([&] {
      lipp_.bulk_load(loading_data.data(), loading_data.size());
    });

    initial_count_ = data.size();
    // At least 1 key must accumulate before flushing; minimum threshold = 1000
    // to avoid pathological flush-per-insert behaviour on tiny initial sets.
    flush_threshold_ = std::max(
        size_t(1000),
        initial_count_ * flush_threshold_per_mille / 1000);
    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    // Check the (small) DPGM buffer first.
    auto it = dpgm_.find(lookup_key);
    if (it != dpgm_.end()) {
      return static_cast<size_t>(it->value());
    }

    // Fall back to the main LIPP store.
    uint64_t value;
    if (lipp_.find(lookup_key, value)) {
      return static_cast<size_t>(value);
    }

    return util::OVERFLOW;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    dpgm_.insert(data.key, data.value);
    ++dpgm_count_;

    if (dpgm_count_ >= flush_threshold_) {
      Flush();
    }
  }

  std::string name() const { return "HybridPGMLipp"; }

  std::size_t size() const {
    return dpgm_.size_in_bytes() + lipp_.index_size();
  }

  bool applicable(bool unique, bool range_query, bool insert, bool multithread,
                  const std::string& ops_filename) const {
    // Requires unique keys (LIPP constraint) and single-threaded execution.
    std::string sname = SearchClass::name();
    return unique && !multithread && sname != "LinearAVX";
  }

  std::vector<std::string> variants() const {
    return {SearchClass::name(), std::to_string(pgm_error),
            std::to_string(flush_threshold_per_mille)};
  }

 private:
  // Drain all entries from DPGM into LIPP one by one, then reset DPGM.
  // Uses for_each (level-by-level, unordered) instead of the sorted iterator
  // because ordering does not matter for migration and avoids the buggy begin().
  void Flush() {
    dpgm_.for_each([this](const KeyType& k, uint64_t v) {
      lipp_.insert(k, v);
    });
    dpgm_      = DPGMType{};
    dpgm_count_ = 0;
  }

  DPGMType             dpgm_;
  LIPP<KeyType, uint64_t> lipp_;
  size_t initial_count_   = 0;
  size_t dpgm_count_      = 0;
  size_t flush_threshold_ = 0;
};

#endif  // TLI_HYBRID_PGM_LIPP_H
