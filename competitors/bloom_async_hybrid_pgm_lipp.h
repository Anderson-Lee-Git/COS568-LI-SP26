#ifndef TLI_BLOOM_ASYNC_HYBRID_PGM_LIPP_H
#define TLI_BLOOM_ASYNC_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>

#include "../util.h"
#include "base.h"
#include "pgm_index_dynamic.hpp"
#include "./lipp/src/core/lipp.h"

template <class KeyType>
class BloomAsyncHybridFilter {
 public:
  BloomAsyncHybridFilter() = default;

  explicit BloomAsyncHybridFilter(size_t expected_items,
                                  double bits_per_item = 10.0) {
    Reset(expected_items, bits_per_item);
  }

  void Reset(size_t expected_items, double bits_per_item) {
    expected_items = std::max<size_t>(expected_items, 1);
    bit_count_ = std::max<size_t>(
        64, static_cast<size_t>(expected_items * bits_per_item));
    bit_count_ = ((bit_count_ + 63) / 64) * 64;
    bits_.assign(bit_count_ / 64, 0);
    hash_count_ = std::max<size_t>(
        1, static_cast<size_t>(bits_per_item * 0.6931471805599453 + 0.5));
  }

  void Add(const KeyType& key) {
    if (bit_count_ == 0) {
      return;
    }

    const uint64_t h1 = SplitMix64(static_cast<uint64_t>(key));
    const uint64_t h2 = SplitMix64(static_cast<uint64_t>(key) +
                                   0x9e3779b97f4a7c15ULL) | 1ULL;
    for (size_t i = 0; i < hash_count_; ++i) {
      const size_t bit = static_cast<size_t>((h1 + i * h2) % bit_count_);
      bits_[bit >> 6] |= (uint64_t{1} << (bit & 63));
    }
  }

  bool MightContain(const KeyType& key) const {
    if (bit_count_ == 0) {
      return false;
    }

    const uint64_t h1 = SplitMix64(static_cast<uint64_t>(key));
    const uint64_t h2 = SplitMix64(static_cast<uint64_t>(key) +
                                   0x9e3779b97f4a7c15ULL) | 1ULL;
    for (size_t i = 0; i < hash_count_; ++i) {
      const size_t bit = static_cast<size_t>((h1 + i * h2) % bit_count_);
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

  std::vector<uint64_t> bits_;
  size_t bit_count_ = 0;
  size_t hash_count_ = 0;
};

// Async hybrid variant with a Bloom fast path for DPGM-resident insert buffers.
//
// LIPP stores the bulk-loaded base data. Foreground inserts go to an active DPGM
// and are recorded in active_filter_. Lookups probe active/snapshot DPGMs only
// when the corresponding Bloom filter says the key may be in that buffer; most
// base-key and negative lookups can go straight to LIPP.
template <class KeyType, class SearchClass, size_t pgm_error = 64,
          size_t flush_threshold_per_mille = 5,
          size_t bloom_bits_per_key = 10>
class BloomAsyncHybridPGMLipp : public Competitor<KeyType, SearchClass> {
  using DPGMType = DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                                   PGMIndex<KeyType, SearchClass, pgm_error, 16>>;
  using BloomFilter = BloomAsyncHybridFilter<KeyType>;

 public:
  explicit BloomAsyncHybridPGMLipp(const std::vector<int>& params) {}

  ~BloomAsyncHybridPGMLipp() {
    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      shutdown_ = true;
    }
    flush_cv_.notify_one();

    if (flush_thread_.joinable()) {
      flush_thread_.join();
    }
  }

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
    active_filter_ = BloomFilter(flush_threshold_ * 2, bloom_bits_per_key);

    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      shutdown_ = false;
      flush_requested_ = false;
      flush_in_progress_ = false;
      snapshot_dpgm_.reset();
      snapshot_filter_.reset();
    }

    flush_thread_ = std::thread(&BloomAsyncHybridPGMLipp::FlushWorker, this);
    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    if (active_filter_.MightContain(lookup_key)) {
      auto it = active_dpgm_.find(lookup_key);
      if (it != active_dpgm_.end()) {
        return static_cast<size_t>(it->value());
      }
    }

    std::shared_ptr<const DPGMType> snapshot;
    std::shared_ptr<const BloomFilter> snapshot_filter;
    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      snapshot = snapshot_dpgm_;
      snapshot_filter = snapshot_filter_;
    }

    if (snapshot && snapshot_filter &&
        snapshot_filter->MightContain(lookup_key)) {
      auto snapshot_it = snapshot->find(lookup_key);
      if (snapshot_it != snapshot->end()) {
        return static_cast<size_t>(snapshot_it->value());
      }
    }

    uint64_t value;
    {
      std::shared_lock<std::shared_mutex> lock(lipp_mutex_);
      if (lipp_.find(lookup_key, value)) {
        return static_cast<size_t>(value);
      }
    }

    return util::OVERFLOW;
  }

  void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
    active_dpgm_.insert(data.key, data.value);
    active_filter_.Add(data.key);
    ++active_count_;

    if (active_count_ >= flush_threshold_) {
      MaybeStartFlush();
    }
  }

  std::string name() const { return "BloomAsyncHybridPGMLipp"; }

  std::size_t size() const {
    size_t total = active_dpgm_.size_in_bytes();
    total += active_filter_.size_in_bytes();

    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      if (snapshot_dpgm_) {
        total += snapshot_dpgm_->size_in_bytes();
      }
      if (snapshot_filter_) {
        total += snapshot_filter_->size_in_bytes();
      }
    }

    {
      std::shared_lock<std::shared_mutex> lock(lipp_mutex_);
      total += lipp_.index_size();
    }

    return total;
  }

  bool applicable(bool unique, bool range_query, bool insert, bool multithread,
                  const std::string& ops_filename) const {
    std::string sname = SearchClass::name();
    return unique && !multithread && sname != "LinearAVX";
  }

  std::vector<std::string> variants() const {
    return {SearchClass::name(), std::to_string(pgm_error),
            std::to_string(flush_threshold_per_mille),
            std::to_string(bloom_bits_per_key)};
  }

 private:
  static constexpr size_t kFlushBatchSize = 1024;

  void MaybeStartFlush() {
    std::unique_lock<std::mutex> lock(snapshot_mutex_);

    if (flush_in_progress_) {
      return;
    }

    auto snapshot = std::make_shared<DPGMType>(std::move(active_dpgm_));
    auto filter_snapshot =
        std::make_shared<BloomFilter>(std::move(active_filter_));
    active_dpgm_ = DPGMType{};
    active_filter_ = BloomFilter(flush_threshold_ * 2, bloom_bits_per_key);
    active_count_ = 0;

    snapshot_dpgm_ = snapshot;
    snapshot_filter_ = filter_snapshot;
    flush_in_progress_ = true;
    flush_requested_ = true;

    lock.unlock();
    flush_cv_.notify_one();
  }

  void FlushWorker() {
    while (true) {
      std::shared_ptr<const DPGMType> snapshot;

      {
        std::unique_lock<std::mutex> lock(snapshot_mutex_);
        flush_cv_.wait(lock, [&] {
          return shutdown_ || flush_requested_;
        });

        if (shutdown_ && !flush_requested_ && !snapshot_dpgm_) {
          return;
        }

        snapshot = snapshot_dpgm_;
        flush_requested_ = false;
      }

      if (snapshot) {
        FlushSnapshotToLipp(snapshot);

        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        if (snapshot_dpgm_ == snapshot) {
          snapshot_dpgm_.reset();
          snapshot_filter_.reset();
        }
        flush_in_progress_ = false;

        if (shutdown_) {
          return;
        }
      }
    }
  }

  void FlushSnapshotToLipp(std::shared_ptr<const DPGMType> snapshot) {
    std::vector<std::pair<KeyType, uint64_t>> batch;
    batch.reserve(kFlushBatchSize);

    snapshot->for_each([&](const KeyType& k, uint64_t v) {
      batch.emplace_back(k, v);

      if (batch.size() >= kFlushBatchSize) {
        FlushBatch(batch);
        batch.clear();
      }
    });

    if (!batch.empty()) {
      FlushBatch(batch);
    }
  }

  void FlushBatch(const std::vector<std::pair<KeyType, uint64_t>>& batch) {
    std::unique_lock<std::shared_mutex> lock(lipp_mutex_);
    for (const auto& kv : batch) {
      lipp_.insert(kv.first, kv.second);
    }
  }

  DPGMType active_dpgm_;
  BloomFilter active_filter_;
  std::shared_ptr<const DPGMType> snapshot_dpgm_;
  std::shared_ptr<const BloomFilter> snapshot_filter_;
  LIPP<KeyType, uint64_t> lipp_;

  std::thread flush_thread_;
  mutable std::mutex snapshot_mutex_;
  std::condition_variable flush_cv_;
  mutable std::shared_mutex lipp_mutex_;

  size_t initial_count_ = 0;
  size_t active_count_ = 0;
  size_t flush_threshold_ = 0;

  bool flush_requested_ = false;
  bool flush_in_progress_ = false;
  bool shutdown_ = false;
};

#endif  // TLI_BLOOM_ASYNC_HYBRID_PGM_LIPP_H
