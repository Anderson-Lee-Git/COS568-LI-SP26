# Task 2 - Milestone 3 Plan: Non-Async Hybrid With Bloom Filter Fast Path

**Goal:** implement a Bloom-filtered version of the Milestone 2 synchronous
hybrid, without the Milestone 3 async flush machinery. The target workload is the
lookup-heavy mixed workload, where LIPP currently beats the async+Bloom hybrid
because the async version adds per-lookup lock, snapshot, and thread-management
overhead.

This plan is intentionally separate from:

- `task_2_milestone_2_plan.md`
- `task_2_milestone_3_plan.md`
- `task_2_milestone_3_plan_with_bloom_filter.md`

---

## 1. Motivation

The current result set in `results/` compares only:

```text
LIPP
BloomAsyncHybridPGMLipp
```

on the lookup-heavy workload:

```text
ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix
```

The async+Bloom design does not beat LIPP. The likely reason is not only DPGM
probe overhead, but also the fixed async lookup overhead:

```text
lookup key
  -> active Bloom check
  -> maybe active DPGM
  -> lock snapshot mutex
  -> copy snapshot shared_ptrs
  -> maybe snapshot Bloom check
  -> maybe snapshot DPGM
  -> shared_lock LIPP
  -> LIPP lookup
```

For lookup-heavy:

```text
total operations = 2,000,000
insert count     = about 200,000
lookup count     = about 1,800,000
negative lookups = about 900,000
positive lookups = about 900,000
```

That means any extra lookup-path overhead is paid about 1.8M times. The async
flush worker can remove foreground migration pauses, but it also forces LIPP
lookups to use synchronization because the worker may mutate LIPP concurrently.
For a lookup-heavy workload, this trade-off is bad.

The new design removes async entirely:

```text
lookup key
  -> active Bloom check
  -> maybe DPGM
  -> LIPP
```

No snapshot DPGM, no background thread, no snapshot mutex, no shared pointer
copies, and no LIPP lock are needed because the benchmark operation stream is
single-threaded.

---

## 2. Relationship To Previous Designs

### 2.1 Milestone 2 Hybrid

`HybridPGMLipp` already has the right ownership model:

- LIPP stores the bulk-loaded initial data.
- DPGM receives foreground inserts.
- Lookups check DPGM first, then LIPP.
- When DPGM reaches a threshold, `Flush()` synchronously inserts DPGM entries
  into LIPP and resets DPGM.
- No locks are needed because all operations are foreground-thread operations.

The weakness is lookup-heavy performance:

```text
Every lookup probes DPGM, even if the key was never inserted.
```

That is especially expensive for:

- bulk-loaded positive lookups, because the key is already in LIPP;
- negative lookups, because the key is in neither structure;
- lookup-heavy workloads, because these cases dominate the operation stream.

### 2.2 Milestone 3 Async Hybrid

`AsyncHybridPGMLipp` changes flushing:

- Active DPGM receives inserts.
- When full, active DPGM is moved into an immutable snapshot.
- A background worker drains the snapshot into LIPP.
- Foreground lookups check active DPGM, snapshot DPGM, then LIPP.
- LIPP access requires a shared/exclusive lock because lookups can race with
  worker inserts.

This removes foreground flush pauses, but it adds lookup-path overhead.

### 2.3 Async Hybrid With Bloom Filter

`BloomAsyncHybridPGMLipp` adds:

- `active_filter_`
- `snapshot_filter_`
- Bloom-gated DPGM probes

This is directionally correct, but the async lookup path still pays:

- active Bloom hash probes;
- snapshot mutex acquisition;
- shared pointer copies;
- optional snapshot Bloom hash probes;
- LIPP shared lock;
- occasional interference with background flush writes.

For lookup-heavy, this overhead is paid too often.

### 2.4 Proposed Non-Async Bloom Hybrid

The new design keeps the good part of Milestone 2 and the good part of
async+Bloom:

```text
Milestone 2:
  simple single-threaded hybrid, no locks, no worker

Async+Bloom:
  Bloom filter avoids unnecessary DPGM probes

New variant:
  simple single-threaded hybrid + Bloom-gated DPGM lookup
```

---

## 3. High-Level Design

Create a new competitor:

```text
BloomHybridPGMLipp
```

Core state:

```cpp
DPGMType dpgm_;
BloomFilter filter_;
LIPP<KeyType, uint64_t> lipp_;

size_t initial_count_ = 0;
size_t dpgm_count_ = 0;
size_t flush_threshold_ = 0;
```

No async state:

```text
No std::thread
No std::mutex
No std::shared_mutex
No std::condition_variable
No snapshot_dpgm_
No snapshot_filter_
```

Lookup path:

```text
if Bloom says key may be in DPGM:
  probe DPGM
  if found, return value

probe LIPP
if found, return value

return OVERFLOW
```

Insert path:

```text
insert key into DPGM
add key to Bloom filter
increment dpgm_count_

if dpgm_count_ reaches threshold:
  synchronously flush DPGM into LIPP
  reset DPGM
  reset Bloom filter
```

For lookup-heavy benchmarking, choose thresholds that avoid flushing during the
2M-op workload. That preserves cheap DPGM inserts and avoids a large foreground
flush pause. The Bloom filter makes the growing DPGM tolerable by skipping it for
most lookups.

---

## 4. Expected Workload Behavior

The lookup-heavy workload uses:

```text
insert ratio = 0.1
lookup ratio = 0.9
negative lookup ratio = 0.5
```

For the 100M datasets:

```text
initial_count_ ~= 99.8M after held-out insert keys
total inserts  ~= 200K
```

With the current per-mille threshold formula:

```cpp
flush_threshold_ = max(size_t(1000),
                       initial_count_ * flush_threshold_per_mille / 1000);
```

Approximate thresholds:

```text
1 per-mille  ~= 100K keys
2 per-mille  ~= 200K keys
3 per-mille  ~= 300K keys
5 per-mille  ~= 500K keys
10 per-mille ~= 1M keys
20 per-mille ~= 2M keys
```

For lookup-heavy, avoid `1` and usually avoid `2`:

- `1‰` flushes around halfway through the inserted keys.
- `2‰` may flush near the end of the workload.
- `3‰`, `5‰`, `10‰`, and `20‰` should usually avoid flush entirely.

That gives the intended behavior:

```text
200K inserts:
  DPGM absorbs all inserts
  Bloom filter tracks all inserted keys
  LIPP is never mutated after build

1.8M lookups:
  most bulk-loaded and negative keys are Bloom negatives
  Bloom negatives skip DPGM and go directly to LIPP
  only inserted-key lookups and Bloom false positives probe DPGM
```

---

## 5. Why This Can Beat The Async+Bloom Variant

Compared with `BloomAsyncHybridPGMLipp`, this removes:

- snapshot DPGM lookup path;
- snapshot Bloom filter lookup path;
- snapshot mutex acquisition on every lookup;
- `shared_ptr` refcount traffic on every lookup;
- LIPP shared lock on every lookup;
- background flush write-lock interference;
- destructor thread join overhead;
- condition variable state.

The common lookup path becomes:

```text
Bloom negative:
  Bloom check + LIPP lookup

Bloom positive:
  Bloom check + DPGM lookup + LIPP lookup on DPGM miss
```

This is the cheapest hybrid lookup path that still preserves correctness for
recently inserted keys.

---

## 6. Why This Might Beat LIPP

LIPP has the best direct lookup path:

```text
lookup -> LIPP
insert -> LIPP
```

The Bloom hybrid can only beat LIPP if insertion savings outweigh lookup tax:

```text
savings from 200K DPGM inserts
  >
extra Bloom work on 1.8M lookups
```

Therefore the implementation must aggressively minimize Bloom overhead.

Important implication:

```text
The Bloom filter should not simply copy the async+Bloom settings.
```

The async+Bloom implementation uses bits/key values such as `8` and `10`, which
derive about `6` or `7` hash probes per lookup. That may be too expensive when
the filter is checked 1.8M times.

This variant should test smaller, faster filters:

```text
bits/key = 2, 4, 6, 8
hashes   = 2, 3, 4, 5 or 6 depending on policy
```

A slightly higher false-positive rate may be acceptable if the Bloom check itself
is much faster. The DPGM contains at most about 200K keys in the lookup-heavy
target, so occasional false-positive DPGM probes may cost less than doing too
many Bloom bit probes on every lookup.

---

## 7. Bloom Filter Design

The current `BloomAsyncHybridFilter` is correct, but it is not necessarily tuned
for fastest lookup:

```cpp
bit = (h1 + i * h2) % bit_count_;
```

Modulo on every hash probe can be expensive. For this non-async lookup-heavy
variant, prefer a power-of-two bit count and use a mask:

```cpp
bit = (h1 + i * h2) & bit_mask_;
```

Recommended helper:

```cpp
template <class KeyType>
class BloomHybridFilter {
 public:
  BloomHybridFilter() = default;
  BloomHybridFilter(size_t expected_items,
                    size_t bits_per_item,
                    size_t hash_count) {
    Reset(expected_items, bits_per_item, hash_count);
  }

  void Reset(size_t expected_items,
             size_t bits_per_item,
             size_t hash_count);

  void Add(const KeyType& key);
  bool MightContain(const KeyType& key) const;
  size_t size_in_bytes() const;

 private:
  static uint64_t SplitMix64(uint64_t x);
  static size_t NextPowerOfTwo(size_t x);

  std::vector<uint64_t> bits_;
  size_t bit_count_ = 0;
  size_t bit_mask_ = 0;
  size_t hash_count_ = 0;
};
```

`Reset()`:

```cpp
void Reset(size_t expected_items,
           size_t bits_per_item,
           size_t hash_count) {
  expected_items = std::max<size_t>(expected_items, 1);
  hash_count_ = std::max<size_t>(hash_count, 1);

  size_t requested_bits = expected_items * std::max<size_t>(bits_per_item, 1);
  requested_bits = std::max<size_t>(requested_bits, 64);
  bit_count_ = NextPowerOfTwo(requested_bits);
  bit_mask_ = bit_count_ - 1;
  bits_.assign(bit_count_ / 64, 0);
}
```

`Add()`:

```cpp
void Add(const KeyType& key) {
  const uint64_t x = static_cast<uint64_t>(key);
  const uint64_t h1 = SplitMix64(x);
  const uint64_t h2 = SplitMix64(x + 0x9e3779b97f4a7c15ULL) | 1ULL;

  for (size_t i = 0; i < hash_count_; ++i) {
    const size_t bit = static_cast<size_t>(h1 + i * h2) & bit_mask_;
    bits_[bit >> 6] |= (uint64_t{1} << (bit & 63));
  }
}
```

`MightContain()`:

```cpp
bool MightContain(const KeyType& key) const {
  if (bit_count_ == 0) {
    return false;
  }

  const uint64_t x = static_cast<uint64_t>(key);
  const uint64_t h1 = SplitMix64(x);
  const uint64_t h2 = SplitMix64(x + 0x9e3779b97f4a7c15ULL) | 1ULL;

  for (size_t i = 0; i < hash_count_; ++i) {
    const size_t bit = static_cast<size_t>(h1 + i * h2) & bit_mask_;
    if ((bits_[bit >> 6] & (uint64_t{1} << (bit & 63))) == 0) {
      return false;
    }
  }

  return true;
}
```

Important details:

- `bit_count_` must be a power of two.
- The number of `uint64_t` words is still `bit_count_ / 64`.
- `hash_count_` should be a template parameter or compile-time constant in each
  benchmark variant.
- The filter must be reset after synchronous flush, because all flushed DPGM keys
  are now in LIPP.

---

## 8. Core Index API

Create:

```text
competitors/bloom_hybrid_pgm_lipp.h
```

Template:

```cpp
template <class KeyType,
          class SearchClass,
          size_t pgm_error = 64,
          size_t flush_threshold_per_mille = 5,
          size_t bloom_bits_per_key = 4,
          size_t bloom_hash_count = 3>
class BloomHybridPGMLipp : public Competitor<KeyType, SearchClass> {
  using DPGMType = DynamicPGMIndex<
      KeyType,
      uint64_t,
      SearchClass,
      PGMIndex<KeyType, SearchClass, pgm_error, 16>>;

  using BloomFilter = BloomHybridFilter<KeyType>;

 public:
  explicit BloomHybridPGMLipp(const std::vector<int>& params) {}

  uint64_t Build(const std::vector<KeyValue<KeyType>>& data,
                 size_t num_threads);

  size_t EqualityLookup(const KeyType& lookup_key,
                        uint32_t thread_id) const;

  void Insert(const KeyValue<KeyType>& data,
              uint32_t thread_id);

  std::string name() const { return "BloomHybridPGMLipp"; }

  std::size_t size() const;

  bool applicable(bool unique,
                  bool range_query,
                  bool insert,
                  bool multithread,
                  const std::string& ops_filename) const;

  std::vector<std::string> variants() const;

 private:
  void Flush();
  void ResetFilter();

  DPGMType dpgm_;
  BloomFilter filter_;
  LIPP<KeyType, uint64_t> lipp_;

  size_t initial_count_ = 0;
  size_t dpgm_count_ = 0;
  size_t flush_threshold_ = 0;
};
```

---

## 9. Build Path

Same as `HybridPGMLipp`, but initialize the Bloom filter:

```cpp
uint64_t Build(const std::vector<KeyValue<KeyType>>& data,
               size_t num_threads) {
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
```

Filter sizing:

```cpp
void ResetFilter() {
  filter_ = BloomFilter(flush_threshold_,
                        bloom_bits_per_key,
                        bloom_hash_count);
}
```

For the lookup-heavy workload, `flush_threshold_` should be larger than the
expected insert count. That means the filter is sized for the maximum DPGM
occupancy during the run.

---

## 10. Lookup Path

The lookup path should be as small as possible:

```cpp
size_t EqualityLookup(const KeyType& lookup_key,
                      uint32_t thread_id) const {
  if (filter_.MightContain(lookup_key)) {
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
```

No locks are needed:

- no background worker mutates LIPP;
- the benchmark operation stream is single-threaded;
- DPGM and Bloom filter are mutated only by foreground inserts;
- lookup and insert are not concurrent in the single-threaded benchmark.

Potential micro-optimization:

```cpp
if (dpgm_count_ != 0 && filter_.MightContain(lookup_key)) {
  ...
}
```

This avoids hashing while the DPGM is empty. It matters early in the workload and
after any synchronous flush.

Preferred lookup path:

```cpp
if (dpgm_count_ != 0 && filter_.MightContain(lookup_key)) {
  ...
}
```

---

## 11. Insert Path

Insert into DPGM and Bloom filter:

```cpp
void Insert(const KeyValue<KeyType>& data,
            uint32_t thread_id) {
  dpgm_.insert(data.key, data.value);
  filter_.Add(data.key);
  ++dpgm_count_;

  if (dpgm_count_ >= flush_threshold_) {
    Flush();
  }
}
```

The order can be either:

```text
DPGM insert, then Bloom Add
```

or:

```text
Bloom Add, then DPGM insert
```

Because the workload is single-threaded, no foreground lookup can observe the
intermediate state. Use DPGM first for consistency with `HybridPGMLipp`.

---

## 12. Flush Path

Keep the Milestone 2 synchronous flush:

```cpp
void Flush() {
  dpgm_.for_each([this](const KeyType& k, uint64_t v) {
    lipp_.insert(k, v);
  });

  dpgm_ = DPGMType{};
  dpgm_count_ = 0;
  ResetFilter();
}
```

Correctness:

- before flush, inserted keys are in DPGM and Bloom filter;
- during synchronous flush, no concurrent lookup happens;
- after flush, inserted keys are in LIPP;
- DPGM is empty;
- Bloom filter is reset, so old DPGM keys no longer cause false-positive DPGM
  probes.

For lookup-heavy optimization, the selected threshold should normally avoid this
path during the measured operation stream.

---

## 13. Size Reporting

Report all memory:

```cpp
std::size_t size() const {
  return dpgm_.size_in_bytes()
       + filter_.size_in_bytes()
       + lipp_.index_size();
}
```

Expected Bloom memory for the lookup-heavy workload:

```text
flush threshold 3‰ on 100M  ~= 300K expected keys
4 bits/key                  ~= 1.2M bits ~= 150 KB
8 bits/key                  ~= 2.4M bits ~= 300 KB
```

This is tiny relative to the LIPP index, but it should still be included.

---

## 14. Benchmark Wiring

Create:

```text
benchmarks/benchmark_bloom_hybrid_pgm_lipp.h
benchmarks/benchmark_bloom_hybrid_pgm_lipp.cc
```

Header:

```cpp
#pragma once
#include "benchmark.h"

template <typename Searcher>
void benchmark_64_bloom_hybrid_pgm_lipp(
    tli::Benchmark<uint64_t>& benchmark,
    bool pareto,
    const std::vector<int>& params);

template <int record>
void benchmark_64_bloom_hybrid_pgm_lipp(
    tli::Benchmark<uint64_t>& benchmark,
    const std::string& filename);
```

Register the benchmark source in `CMakeLists.txt`:

```cmake
"benchmarks/benchmark_bloom_hybrid_pgm_lipp.cc"
```

Register the index in `benchmark.cc`:

```cpp
#include "benchmarks/benchmark_bloom_hybrid_pgm_lipp.h"
```

In the search-class dispatch:

```cpp
check_only("BloomHybridPGMLipp",
           benchmark_64_bloom_hybrid_pgm_lipp<SearchClass>(
               benchmark, pareto, params));
```

In the default dispatch:

```cpp
check_only("BloomHybridPGMLipp",
           benchmark_64_bloom_hybrid_pgm_lipp<record>(
               benchmark, filename));
```

---

## 15. Hyperparameter Plan

The goal is not to minimize false positives at all costs. The goal is to maximize
lookup-heavy mixed throughput.

That means:

```text
avoid flushing
minimize Bloom lookup cost
accept some false positives if the filter is faster
```

### 15.1 Primary Lookup-Heavy Sweep

Use these variants first:

```text
pgm_error: 64
flush threshold per-mille: 3, 5, 10, 20
Bloom bits/key: 2, 4, 6, 8
Bloom hash count: 2, 3, 4
```

Recommended initial matrix:

```text
flush=3,  bits/key=2, hashes=2
flush=3,  bits/key=4, hashes=3
flush=3,  bits/key=6, hashes=4
flush=5,  bits/key=2, hashes=2
flush=5,  bits/key=4, hashes=3
flush=5,  bits/key=6, hashes=4
flush=10, bits/key=2, hashes=2
flush=10, bits/key=4, hashes=3
flush=10, bits/key=6, hashes=4
flush=20, bits/key=4, hashes=3
```

Why this matrix:

- `3‰` is the smallest threshold that should avoid flush for ~200K inserts on
  100M-key datasets.
- `5‰` and `10‰` give more headroom if the bulk-loaded count is smaller than
  expected.
- `20‰` checks whether a larger, less full DPGM/filter changes performance.
- `2` bits/key with `2` hashes tests the fastest but noisiest Bloom filter.
- `4` bits/key with `3` hashes is the likely sweet spot.
- `6` bits/key with `4` hashes checks whether lower false positives are worth
  the additional bit probes.

### 15.2 Secondary Sweep

If the primary sweep is close to LIPP, try:

```text
pgm_error: 128
flush threshold per-mille: 3, 5, 10
Bloom bits/key: 2, 4
Bloom hash count: 2, 3
```

This checks whether a more permissive DPGM model reduces DPGM lookup cost on true
positives and false positives.

### 15.3 Avoid For Lookup-Heavy

Avoid these as default lookup-heavy choices:

```text
flush=1
flush=2
bits/key=10
bits/key=12
hashes >= 6
```

Reasons:

- `1‰` and `2‰` can trigger synchronous flushes during the lookup-heavy run.
- large filters with many hashes increase the cost paid by every lookup;
- async+Bloom already suggests that 8-10 bits/key is not enough to beat LIPP when
  the lookup path has additional overhead.

---

## 16. File-Based Benchmark Defaults

For `benchmarks/benchmark_bloom_hybrid_pgm_lipp.cc`, the file-based path should
prioritize lookup-heavy defaults across all three datasets:

```cpp
if (filename.find("0.100000i") != std::string::npos) {
  benchmark.template Run<BloomHybridPGMLipp<
      uint64_t, BranchingBinarySearch<record>, 64, 3, 2, 2>>();
  benchmark.template Run<BloomHybridPGMLipp<
      uint64_t, BranchingBinarySearch<record>, 64, 3, 4, 3>>();
  benchmark.template Run<BloomHybridPGMLipp<
      uint64_t, BranchingBinarySearch<record>, 64, 5, 2, 2>>();
  benchmark.template Run<BloomHybridPGMLipp<
      uint64_t, BranchingBinarySearch<record>, 64, 5, 4, 3>>();
  benchmark.template Run<BloomHybridPGMLipp<
      uint64_t, BranchingBinarySearch<record>, 64, 10, 4, 3>>();
  benchmark.template Run<BloomHybridPGMLipp<
      uint64_t, BranchingBinarySearch<record>, 64, 10, 6, 4>>();
}
```

For insert-heavy, include only a small sanity set because this design is not
optimized for insert-heavy reporting:

```cpp
if (filename.find("0.900000i") != std::string::npos) {
  benchmark.template Run<BloomHybridPGMLipp<
      uint64_t, BranchingBinarySearch<record>, 128, 10, 4, 3>>();
  benchmark.template Run<BloomHybridPGMLipp<
      uint64_t, BranchingBinarySearch<record>, 128, 20, 4, 3>>();
}
```

If final runtime is limited, run only the lookup-heavy workload first.

---

## 17. Script Updates

Update `scripts/run_final_benchmark.sh`.

Recommended lookup-heavy comparison:

```bash
INDEXES=(
    LIPP
    HybridPGMLipp
    BloomHybridPGMLipp
    BloomAsyncHybridPGMLipp
)
```

For focused testing, compare only:

```bash
INDEXES=(
    LIPP
    BloomHybridPGMLipp
)
```

Keep:

```bash
run_mixed "$DATASET" "$WORKLOAD_10I" "$INDEX"
```

Leave insert-heavy commented out until lookup-heavy results are understood:

```bash
# run_mixed "$DATASET" "$WORKLOAD_90I" "$INDEX"
```

CSV header must include the additional variant field:

```text
index_name,build_time_ns1,build_time_ns2,build_time_ns3,index_size_bytes,
mixed_throughput_mops1,mixed_throughput_mops2,mixed_throughput_mops3,
search_method,value,value2,value3,value4
```

For `BloomHybridPGMLipp`, use:

```text
value  = pgm_error
value2 = flush_threshold_per_mille
value3 = bloom_bits_per_key
value4 = bloom_hash_count
```

If changing the global CSV header is inconvenient, keep three variant fields by
encoding bits/key and hash count together in `value3`, but the clearer approach
is to add `value4` and update the analysis script.

---

## 18. Analysis Script Updates

Update `scripts/analysis_final.py`.

Add to `INDEX_MAP`:

```python
"BloomHybridPGMLipp": "Bloom Hybrid",
```

Update display order:

```python
DISPLAY_ORDER = [
    "LIPP",
    "Hybrid",
    "Bloom Hybrid",
    "Bloom Async",
]
```

If retaining all indexes:

```python
DISPLAY_ORDER = [
    "DPGM",
    "LIPP",
    "Naive Hybrid",
    "Async Hybrid",
    "Bloom Hybrid",
    "Bloom Async",
]
```

Add a `value4` field if the CSV header is expanded:

```python
CSV_COLS = [
    "index_name",
    "build_time_ns1", "build_time_ns2", "build_time_ns3",
    "index_size_bytes",
    "mixed_throughput_mops1",
    "mixed_throughput_mops2",
    "mixed_throughput_mops3",
    "search_method",
    "value",
    "value2",
    "value3",
    "value4",
]
```

Extend the `Result` dataclass if needed:

```python
value4: float | None
```

The analysis should still choose the best row by average throughput.

---

## 19. Correctness Invariants

The key invariant:

```text
Every inserted key is either in DPGM or in LIPP.
```

Before flush:

```text
inserted key -> DPGM
inserted key -> Bloom filter
```

Lookup:

```text
Bloom true  -> DPGM checked
Bloom false -> DPGM skipped
```

There must be no Bloom false negatives for keys currently in DPGM.

After flush:

```text
all DPGM keys inserted into LIPP
DPGM reset
Bloom filter reset
```

Skipping DPGM after the reset is correct because the keys are now in LIPP.

False positives:

```text
Bloom true for non-DPGM key -> extra DPGM probe -> LIPP fallback
```

False positives hurt performance but do not hurt correctness.

---

## 20. Verification Plan

Build:

```bash
bash scripts/build_benchmark.sh
```

Correctness smoke test on Facebook lookup-heavy:

```bash
build/benchmark \
  ./data/fb_100M_public_uint64 \
  ./data/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix \
  --through --csv --verify --only BloomHybridPGMLipp -r 1
```

Performance smoke test:

```bash
build/benchmark \
  ./data/fb_100M_public_uint64 \
  ./data/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix \
  --through --csv --only BloomHybridPGMLipp -r 3
```

Focused comparison:

```bash
build/benchmark \
  ./data/fb_100M_public_uint64 \
  ./data/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix \
  --through --csv --only LIPP -r 3
```

Then run the same comparison for:

```text
books_100M_public_uint64
osmc_100M_public_uint64
```

Use `sbatch` or an interactive compute node for final numbers. Do not trust
login-node timing.

---

## 21. Optional Instrumentation

If the Bloom hybrid is still slower than LIPP, add counters temporarily:

```cpp
mutable uint64_t lookup_count_ = 0;
mutable uint64_t bloom_negative_count_ = 0;
mutable uint64_t bloom_positive_count_ = 0;
mutable uint64_t dpgm_hit_count_ = 0;
mutable uint64_t dpgm_miss_after_bloom_count_ = 0;
mutable uint64_t flush_count_ = 0;
```

Expected lookup-heavy shape:

```text
bloom_negative_count_ should be very high
bloom_positive_count_ should be much smaller than lookup_count_
dpgm_hit_count_ should correspond to lookups of already-inserted held-out keys
flush_count_ should be zero for the best lookup-heavy configs
```

If `bloom_positive_count_` is too high:

- increase bits/key;
- increase hash count only if the extra hash cost is worth it;
- check whether the filter is undersized because the threshold is too small.

If `flush_count_` is nonzero:

- increase `flush_threshold_per_mille`;
- avoid `1‰` and `2‰` for lookup-heavy.

If `bloom_negative_count_` is high but throughput is still below LIPP:

- Bloom hashing itself is too expensive;
- try `bits/key=2, hashes=2`;
- ensure the implementation uses power-of-two masking instead of modulo.

---

## 22. Implementation Checklist

- Create `competitors/bloom_hybrid_pgm_lipp.h`.
- Add `BloomHybridFilter`.
- Use power-of-two Bloom bit count and mask-based indexing.
- Add `BloomHybridPGMLipp`.
- Keep the state single-threaded: `dpgm_`, `filter_`, `lipp_`.
- Initialize `filter_` in `Build()`.
- Add inserted keys to `filter_` in `Insert()`.
- Gate DPGM lookup behind `dpgm_count_ != 0 && filter_.MightContain(key)`.
- Keep synchronous `Flush()` from `HybridPGMLipp`.
- Reset the Bloom filter after every flush.
- Include Bloom memory in `size()`.
- Return variants:
  `search_method`, `pgm_error`, `flush_threshold_per_mille`,
  `bloom_bits_per_key`, `bloom_hash_count`.
- Create `benchmarks/benchmark_bloom_hybrid_pgm_lipp.h`.
- Create `benchmarks/benchmark_bloom_hybrid_pgm_lipp.cc`.
- Add the new benchmark source to `CMakeLists.txt`.
- Include and dispatch `BloomHybridPGMLipp` in `benchmark.cc`.
- Add `BloomHybridPGMLipp` to `scripts/run_final_benchmark.sh`.
- Update CSV header handling if adding `value4`.
- Add `BloomHybridPGMLipp` to `scripts/analysis_final.py`.
- Build.
- Run `--verify` on a lookup-heavy workload.
- Run focused LIPP vs Bloom Hybrid lookup-heavy benchmarks on all datasets.

---

## 23. Success Criteria

Primary success criterion:

```text
BloomHybridPGMLipp beats LIPP on at least one lookup-heavy dataset.
```

Stronger success criterion:

```text
BloomHybridPGMLipp beats LIPP on the average lookup-heavy throughput
across FB, Books, and OSMC.
```

Diagnostic success criterion:

```text
BloomHybridPGMLipp beats BloomAsyncHybridPGMLipp consistently.
```

If it beats async+Bloom but not LIPP, the conclusion is still useful:

```text
async overhead was significant, but the remaining Bloom-per-lookup tax is still
larger than the insertion savings in this workload.
```

That would motivate either a cheaper prefilter or a workload-aware policy that
uses Bloom gating only after enough inserts have accumulated.

