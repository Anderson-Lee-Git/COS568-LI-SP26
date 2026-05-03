#ifndef TLI_BLOOM_HYBRID_PGM_LIPP_H
#define TLI_BLOOM_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "../util.h"
#include "base.h"
#include "pgm_index_dynamic.hpp"
#include "./lipp/src/core/lipp.h"

template <class KeyType>
class BloomHybridFilter {
 public:
  BloomHybridFilter() = default;

  BloomHybridFilter(size_t expected_items, size_t bits_per_item,
                    size_t hash_count) {
    Reset(expected_items, bits_per_item, hash_count);
  }

  void Reset(size_t expected_items, size_t bits_per_item, size_t hash_count) {
    expected_items = std::max<size_t>(expected_items, 1);
    hash_count_ = std::max<size_t>(hash_count, 1);

    size_t requested_bits =
        expected_items * std::max<size_t>(bits_per_item, 1);
    requested_bits = std::max<size_t>(requested_bits, 64);
    bit_count_ = NextPowerOfTwo(requested_bits);
    bit_mask_ = bit_count_ - 1;
    bits_.assign(bit_count_ / 64, 0);
  }

  void Add(const KeyType& key) {
    if (bit_count_ == 0) {
      return;
    }

    const uint64_t x = static_cast<uint64_t>(key);
    const uint64_t h1 = SplitMix64(x);
    const size_t bit = static_cast<size_t>(h1) & bit_mask_;
    bits_[bit >> 6] |= (uint64_t{1} << (bit & 63));

    if (hash_count_ == 1) {
      return;
    }

    const uint64_t h2 = SplitMix64(x + 0x9e3779b97f4a7c15ULL) | 1ULL;
    for (size_t i = 1; i < hash_count_; ++i) {
      const size_t bit = static_cast<size_t>(h1 + i * h2) & bit_mask_;
      bits_[bit >> 6] |= (uint64_t{1} << (bit & 63));
    }
  }

  bool MightContain(const KeyType& key) const {
    if (bit_count_ == 0) {
      return false;
    }

    const uint64_t x = static_cast<uint64_t>(key);
    const uint64_t h1 = SplitMix64(x);
    const size_t bit = static_cast<size_t>(h1) & bit_mask_;
    if ((bits_[bit >> 6] & (uint64_t{1} << (bit & 63))) == 0) {
      return false;
    }

    if (hash_count_ == 1) {
      return true;
    }

    const uint64_t h2 = SplitMix64(x + 0x9e3779b97f4a7c15ULL) | 1ULL;
    for (size_t i = 1; i < hash_count_; ++i) {
      const size_t bit = static_cast<size_t>(h1 + i * h2) & bit_mask_;
      if ((bits_[bit >> 6] & (uint64_t{1} << (bit & 63))) == 0) {
        return false;
      }
    }

    return true;
  }

  size_t size_in_bytes() const {
    return bits_.size() * sizeof(uint64_t);
  }

 private:
  static uint64_t SplitMix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
  }

  static size_t NextPowerOfTwo(size_t x) {
    --x;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1) {
      x |= x >> shift;
    }
    return x + 1;
  }

  std::vector<uint64_t> bits_;
  size_t bit_count_ = 0;
  size_t bit_mask_ = 0;
  size_t hash_count_ = 0;
};

// Single-threaded synchronous hybrid with a Bloom fast path for the DPGM buffer.
//
// LIPP stores the bulk-loaded base data. Foreground inserts go to DPGM and are
// recorded in filter_. Lookups only probe DPGM when the Bloom filter says the key
// may be buffered, avoiding most DPGM misses on lookup-heavy mixed workloads.
template <class KeyType, class SearchClass, size_t pgm_error = 64,
          size_t flush_threshold_per_mille = 5,
          size_t bloom_bits_per_key = 4, size_t bloom_hash_count = 3>
class BloomHybridPGMLipp : public Competitor<KeyType, SearchClass> {
  using DPGMType = DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                                   PGMIndex<KeyType, SearchClass, pgm_error, 16>>;
  using BloomFilter = BloomHybridFilter<KeyType>;

 public:
  explicit BloomHybridPGMLipp(const std::vector<int>& params) {}

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
    flush_threshold_ = std::max(
        size_t(1000),
        initial_count_ * flush_threshold_per_mille / 1000);
    ResetFilter();
    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    if (dpgm_count_ != 0 && filter_.MightContain(lookup_key)) {
      auto it = dpgm_.find(lookup_key);
      if (it != dpgm_.end()) {
        return static_cast<size_t>(it->value());
      }
    }

    uint64_t value;
    if (lipp_.find(lookup_key, value)) {
      return static_cast<size_t>(value);
    }

    return util::OVERFLOW;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    dpgm_.insert(data.key, data.value);
    filter_.Add(data.key);
    ++dpgm_count_;

    if (dpgm_count_ >= flush_threshold_) {
      Flush();
    }
  }

  std::string name() const { return "BloomHybridPGMLipp"; }

  std::size_t size() const {
    return dpgm_.size_in_bytes() + filter_.size_in_bytes() + lipp_.index_size();
  }

  bool applicable(bool unique, bool range_query, bool insert, bool multithread,
                  const std::string& ops_filename) const {
    std::string sname = SearchClass::name();
    return unique && !multithread && sname != "LinearAVX";
  }

  std::vector<std::string> variants() const {
    return {SearchClass::name(), std::to_string(pgm_error),
            std::to_string(flush_threshold_per_mille),
            std::to_string(bloom_bits_per_key),
            std::to_string(bloom_hash_count)};
  }

 private:
  void Flush() {
    dpgm_.for_each([this](const KeyType& k, uint64_t v) {
      lipp_.insert(k, v);
    });

    dpgm_ = DPGMType{};
    dpgm_count_ = 0;
    ResetFilter();
  }

  void ResetFilter() {
    filter_ = BloomFilter(flush_threshold_, bloom_bits_per_key,
                          bloom_hash_count);
  }

  DPGMType dpgm_;
  BloomFilter filter_;
  LIPP<KeyType, uint64_t> lipp_;

  size_t initial_count_ = 0;
  size_t dpgm_count_ = 0;
  size_t flush_threshold_ = 0;
};

#endif  // TLI_BLOOM_HYBRID_PGM_LIPP_H
