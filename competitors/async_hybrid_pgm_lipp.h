#ifndef TLI_ASYNC_HYBRID_PGM_LIPP_H
#define TLI_ASYNC_HYBRID_PGM_LIPP_H

#include <algorithm>
#include <condition_variable>
#include <cstddef>
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

// Async hybrid index for Milestone 3.
//
// LIPP stores the bulk-loaded base data and remains the primary lookup store.
// active_dpgm_ absorbs foreground inserts. When it reaches the flush threshold,
// the foreground thread publishes it as an immutable snapshot and immediately
// switches to a fresh active DPGM. A background worker drains the snapshot into
// LIPP in batches.
//
// Workload operations are assumed to be single-threaded. The only concurrency is
// between the foreground operation stream and the background flush worker:
//   - active_dpgm_ is foreground-owned and does not need per-operation locking.
//   - snapshot_dpgm_ is immutable and protected by shared_ptr lifetime.
//   - lipp_ uses a shared_mutex because foreground lookups can race with worker
//     inserts.

template <class KeyType, class SearchClass, size_t pgm_error = 64,
          size_t flush_threshold_per_mille = 5>
class AsyncHybridPGMLipp : public Competitor<KeyType, SearchClass> {
  using DPGMType = DynamicPGMIndex<KeyType, uint64_t, SearchClass,
                                   PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

 public:
  explicit AsyncHybridPGMLipp(const std::vector<int>& params) {}

  ~AsyncHybridPGMLipp() {
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

    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      shutdown_ = false;
      flush_requested_ = false;
      flush_in_progress_ = false;
      snapshot_dpgm_.reset();
    }

    flush_thread_ = std::thread(&AsyncHybridPGMLipp::FlushWorker, this);
    return build_time;
  }

  size_t EqualityLookup(const KeyType& lookup_key, uint32_t thread_id) const {
    auto it = active_dpgm_.find(lookup_key);
    if (it != active_dpgm_.end()) {
      return static_cast<size_t>(it->value());
    }

    std::shared_ptr<const DPGMType> snapshot;
    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      snapshot = snapshot_dpgm_;
    }

    if (snapshot) {
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
    ++active_count_;

    if (active_count_ >= flush_threshold_) {
      MaybeStartFlush();
    }
  }

  std::string name() const { return "AsyncHybridPGMLipp"; }

  std::size_t size() const {
    size_t total = active_dpgm_.size_in_bytes();

    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      if (snapshot_dpgm_) {
        total += snapshot_dpgm_->size_in_bytes();
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
            std::to_string(flush_threshold_per_mille)};
  }

 private:
  static constexpr size_t kFlushBatchSize = 1024;

  void MaybeStartFlush() {
    std::unique_lock<std::mutex> lock(snapshot_mutex_);

    // Keep the first version simple: one immutable snapshot can be in flight.
    // If insert-heavy traffic outruns the worker, active_dpgm_ may temporarily
    // grow beyond the threshold, but correctness is preserved.
    if (flush_in_progress_) {
      return;
    }

    auto snapshot = std::make_shared<DPGMType>(std::move(active_dpgm_));
    active_dpgm_ = DPGMType{};
    active_count_ = 0;

    snapshot_dpgm_ = snapshot;
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
  std::shared_ptr<const DPGMType> snapshot_dpgm_;
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

#endif  // TLI_ASYNC_HYBRID_PGM_LIPP_H
