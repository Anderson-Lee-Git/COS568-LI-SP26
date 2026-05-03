# Task 2 - Milestone 3 Plan: Async Hybrid With Bloom Filter Fast Path

**Goal:** improve the lookup-heavy mixed workload by avoiding unnecessary DPGM
probes. Build on the current `AsyncHybridPGMLipp` design, but add a Bloom filter
that predicts whether a lookup key could be in the active/snapshot DPGM buffers.

This plan is intentionally separate from `task_2_milestone_3_plan.md`.

---

## 1. Why The Current Async Hybrid Is Weak On Lookup-Heavy Workloads

The current async hybrid lookup path is:

```text
lookup key
  -> active DPGM
  -> snapshot DPGM, if present
  -> LIPP
```

This is correct, but it hurts the lookup-heavy workload:

- The workload has 2M operations.
- The lookup-heavy workload uses `--insert-ratio 0.1`, so it has about 200K
inserts and 1.8M lookups.
- Half of the lookups are negative lookups because `--negative-lookup-ratio 0.5`.
- Most lookups do not hit the active DPGM.
- Most lookups therefore pay an active-DPGM miss before reaching LIPP.
- During a flush, they may also pay a snapshot-DPGM miss.
- LIPP-only does not pay these DPGM miss probes.

The result is that hybrid lookup throughput can be lower than LIPP even though
hybrid insertions are cheaper.

The core problem:

```text
DPGM lookup is useful only for keys that may have been recently inserted.
But the current lookup path checks DPGM for every key.
```

---

## 2. Workload Details That Matter

The workload generator creates 2M operations per dataset.

From `README.md`:

- `0.100000i` mixed workload: 10% inserts, 90% lookups.
- `0.900000i` mixed workload: 90% inserts, 10% lookups.
- negative lookup ratio is 0.5.

For the lookup-heavy workload:

```text
total operations = 2,000,000
insert count     = 200,000
lookup count     = 1,800,000
positive lookup  = about 900,000
negative lookup  = about 900,000
```

The default insert pattern is `equality`, which appears in workload filenames as
`0m`. In `generate.cc`, `generate_inserts()` handles this by randomly choosing
`insert_cnt` keys from the original data set and removing those keys from the
bulk-loaded set. Those held-out keys later appear as inserts.

Important consequence:

- Inserted keys are not necessarily in a narrow key range.
- A simple min/max inserted-key range is too imprecise.
- Positive lookups are generated from the evolving data set, so some positive
lookups can target inserted keys after those inserts have happened.
- Negative lookups are random keys in the data domain that do not exist.

This makes a Bloom filter attractive: it can test membership in the recently
inserted key set without an expensive DPGM lookup.

---

## 3. Why A Bloom Filter Can Help

A Bloom filter answers:

```text
Could this key be in active/snapshot DPGM?
```

It has:

- No false negatives: if a key was inserted into the filter, lookup will say
"maybe present".
- Some false positives: lookup may say "maybe present" for a key that was never
inserted.

For the hybrid lookup path, this is useful:

```text
if Bloom filter says "definitely not present":
  skip active DPGM and snapshot DPGM
  go directly to LIPP

if Bloom filter says "maybe present":
  check active DPGM
  check snapshot DPGM
  then LIPP
```

This turns the common lookup-heavy case from:

```text
DPGM miss + optional snapshot miss + LIPP lookup
```

into:

```text
Bloom check + LIPP lookup
```

The Bloom check is only a few hash operations and bit tests. It should be much
cheaper than a Dynamic PGM lookup miss.

Expected benefit:

- Most lookup keys target bulk-loaded keys or negative non-inserted keys.
- Those keys are not in the Bloom filter.
- They take the LIPP fast path.
- Only lookups for recently inserted keys, plus false positives, pay DPGM probes.

Trade-off:

- Bloom filter adds work to every lookup.
- Bloom filter adds memory.
- Bloom filter is technically an auxiliary data structure. The README says no
auxiliary data structures other than LIPP and DPGM are allowed. This plan is
therefore an experimental optimization and should only be used if the course
staff permits it or if it is presented clearly as a possible direction.

---

## 4. Design Overview

Build on `competitors/async_hybrid_pgm_lipp.h`.

Current state:

```cpp
DPGMType active_dpgm_;
std::shared_ptr<const DPGMType> snapshot_dpgm_;
LIPP<KeyType, uint64_t> lipp_;
```

Add Bloom filters for the DPGM-resident keys:

```cpp
BloomFilter active_filter_;
std::shared_ptr<const BloomFilter> snapshot_filter_;
```

Ownership model:

- `active_dpgm_` and `active_filter_` are foreground-owned.
- `snapshot_dpgm_` and `snapshot_filter_` are immutable after publication.
- Background worker iterates `snapshot_dpgm_` and inserts keys into LIPP.
- Foreground lookups use `active_filter_` and `snapshot_filter_` to decide
whether DPGM probing is necessary.

Lookup path:

```text
if active_filter says maybe:
  check active_dpgm

copy snapshot_dpgm and snapshot_filter shared_ptrs

if snapshot_filter says maybe:
  check snapshot_dpgm

check LIPP
```

Fast path:

```text
active_filter says no
snapshot_filter says no
=> skip both DPGMs and go directly to LIPP
```

---

## 5. Bloom Filter API

Implement a small internal Bloom filter inside the new index header or in a
dedicated helper header.

For the first implementation, keep it inside
`competitors/async_hybrid_pgm_lipp.h` or create a new competitor variant such as
`competitors/bloom_async_hybrid_pgm_lipp.h`.

Recommended simple API:

```cpp
class BloomFilter {
 public:
  BloomFilter() = default;
  explicit BloomFilter(size_t expected_items, double bits_per_item = 10.0);

  void Add(uint64_t key);
  bool MightContain(uint64_t key) const;
  size_t size_in_bytes() const;

 private:
  std::vector<uint64_t> bits_;
  size_t bit_count_ = 0;
  size_t hash_count_ = 0;
};
```

For `KeyType = uint64_t`, hashing can be simple and fast.

Use double hashing:

```cpp
h1 = SplitMix64(key)
h2 = SplitMix64(key + constant)
for i in 0..hash_count-1:
  bit = (h1 + i * h2) % bit_count
```

Suggested parameters:

- `bits_per_item = 10`
- `hash_count = 7`

For the lookup-heavy workload with 200K inserts:

```text
200K keys * 10 bits/key = 2,000,000 bits = about 250 KB
```

This is tiny compared with LIPP and DPGM index sizes.

Expected false-positive rate with 10 bits/key and 7 hashes is around 1%.

---

## 6. Insert Path

When inserting into active DPGM, also insert into active Bloom filter:

```cpp
void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
  active_dpgm_.insert(data.key, data.value);
  active_filter_.Add(data.key);
  ++active_count_;

  if (active_count_ >= flush_threshold_) {
    MaybeStartFlush();
  }
}
```

Because the workload is single-threaded:

- No per-operation lock is needed for `active_filter_`.
- The foreground thread is the only writer and reader of `active_filter_`,
except during the snapshot handoff.

---

## 7. Snapshot Handoff

When active DPGM reaches the threshold, move both active structures into
immutable snapshot structures:

```cpp
void MaybeStartFlush() {
  std::unique_lock<std::mutex> lock(snapshot_mutex_);

  if (flush_in_progress_) {
    return;
  }

  auto snapshot = std::make_shared<DPGMType>(std::move(active_dpgm_));
  auto filter_snapshot = std::make_shared<BloomFilter>(std::move(active_filter_));

  active_dpgm_ = DPGMType{};
  active_filter_ = BloomFilter(flush_threshold_);
  active_count_ = 0;

  snapshot_dpgm_ = snapshot;
  snapshot_filter_ = filter_snapshot;
  flush_in_progress_ = true;
  flush_requested_ = true;

  lock.unlock();
  flush_cv_.notify_one();
}
```

Important details:

- `snapshot_filter_` must correspond exactly to `snapshot_dpgm_`.
- Both are immutable after publication.
- Lookup copies both shared pointers under the same mutex.
- The worker retires both after all snapshot keys are flushed into LIPP.

---

## 8. Lookup Path

Current async lookup always probes DPGM first. Bloom-filtered lookup should only
probe DPGM if the filter says the key may be present.

Pseudo-code:

```cpp
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
    auto it = snapshot->find(lookup_key);
    if (it != snapshot->end()) {
      return static_cast<size_t>(it->value());
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
```

Correctness:

- Bloom false positives only cause extra DPGM checks.
- Bloom false negatives must not happen for inserted keys.
- If the Bloom implementation is correct, skipping DPGM is safe when the filter
returns false.

---

## 9. Background Flush Worker

The flush worker can remain mostly unchanged.

After it finishes flushing `snapshot_dpgm_` into LIPP, retire both snapshot
objects:

```cpp
{
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  if (snapshot_dpgm_ == snapshot) {
    snapshot_dpgm_.reset();
    snapshot_filter_.reset();
  }
  flush_in_progress_ = false;
}
```

The worker does not need to read the Bloom filter. It only iterates
`snapshot_dpgm_`.

---

## 10. Build-Time Initialization

Initialize the active Bloom filter in `Build()` after computing the flush
threshold:

```cpp
flush_threshold_ = std::max(
    size_t(1000),
    initial_count_ * flush_threshold_per_mille / 1000);

active_filter_ = BloomFilter(flush_threshold_);
snapshot_filter_.reset();
```

If active DPGM is allowed to grow beyond threshold while a flush is in progress,
the filter may receive more than `expected_items`. This increases false positives
but does not break correctness.

For insert-heavy workloads, consider sizing the filter more generously:

```cpp
active_filter_ = BloomFilter(flush_threshold_ * 2);
```

This keeps false-positive rate stable if active DPGM grows during an in-flight
flush.

---

## 11. Size Reporting

Include Bloom filter memory in `size()`:

```cpp
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
```

The Bloom filter memory should be small compared with LIPP, but it should still
be reported honestly.

---

## 12. Hyperparameter Plan

The Bloom filter changes the threshold trade-off.

Without Bloom filter:

- Large thresholds keep many inserts in DPGM.
- Every lookup pays DPGM miss overhead.

With Bloom filter:

- Large thresholds are less harmful because most non-inserted lookup keys skip
DPGM.
- This makes it more reasonable to avoid flushing in lookup-heavy workloads.

Recommended sweep for Bloom async hybrid:

### Lookup-heavy workload: 10% insert, 90% lookup

Total inserts are about 200K.

Try:

```text
flush threshold: 2, 5, 10, 20 per-mille
Bloom bits/key: 8, 10, 12
```

Hypothesis:

- `5` or `10` per-mille may perform well because no flush occurs, or flushing is
rare.
- Bloom filter avoids most active-DPGM miss overhead.
- Insertions remain cheaper than LIPP.

### Insert-heavy workload: 90% insert, 10% lookup

Total inserts are about 1.8M.

Try:

```text
flush threshold: 5, 10, 20 per-mille
Bloom bits/key: 8, 10
```

Hypothesis:

- Bloom helps less because fewer operations are lookups.
- Async flush threshold still matters more.
- `10` per-mille was already the best observed Facebook async threshold, so keep
it in the sweep.

---

## 13. Implementation Strategy

To avoid disrupting the current async implementation, create a new index:

```text
competitors/bloom_async_hybrid_pgm_lipp.h
benchmarks/benchmark_bloom_async_hybrid_pgm_lipp.h
benchmarks/benchmark_bloom_async_hybrid_pgm_lipp.cc
```

Name:

```cpp
BloomAsyncHybridPGMLipp
```

Then register it in:

```text
benchmark.cc
CMakeLists.txt
scripts/run_final_benchmark.sh
scripts/analysis_final.py
```

This allows final comparisons among:

```text
DynamicPGM
LIPP
HybridPGMLipp
AsyncHybridPGMLipp
BloomAsyncHybridPGMLipp
```

---

## 14. Benchmark Expectations

Lookup-heavy workload:

- LIPP has fastest direct lookup path.
- Bloom async hybrid can potentially beat LIPP only if the saved insertion cost
outweighs:
  - Bloom check overhead on every lookup,
  - occasional DPGM checks from true positives and false positives,
  - LIPP lock overhead,
  - optional async flush overhead.

The best Bloom configuration may be one that does little or no flushing in
lookup-heavy workloads:

```text
insert -> DPGM + Bloom
lookup -> Bloom fast path to LIPP for most keys
```

Insert-heavy workload:

- Bloom is less important because lookups are fewer.
- The async flush threshold remains the primary tuning knob.

---

## 15. Risks And Constraints

### False Positives

False positives do not break correctness, but they reduce performance by causing
unnecessary DPGM probes.

### False Negatives

False negatives would break correctness. The implementation must insert every
foreground insert key into `active_filter_` before any lookup can rely on the
filter.

Because workload operations are single-threaded, this is straightforward:

```text
active_dpgm_.insert(key)
active_filter_.Add(key)
```

The ordering can also be:

```text
active_filter_.Add(key)
active_dpgm_.insert(key)
```

Since there are no concurrent foreground lookups during an insert operation,
either ordering is safe in the current benchmark.

### Snapshot Consistency

The snapshot DPGM and snapshot Bloom filter must be published together and
retired together. Otherwise a lookup could skip a DPGM that actually contains
the key.

---

## 16. Implementation Checklist

- Create `BloomFilter` helper with `Add`, `MightContain`, and
`size_in_bytes`.
- Create `BloomAsyncHybridPGMLipp` based on `AsyncHybridPGMLipp`.
- Add `active_filter_`.
- Add `snapshot_filter_`.
- Initialize `active_filter_` in `Build()`.
- Update `Insert()` to add inserted keys to the active filter.
- Update `MaybeStartFlush()` to snapshot both DPGM and Bloom filter.
- Update `EqualityLookup()` to use the Bloom fast path.
- Update flush completion to retire both snapshot objects.
- Include Bloom memory in `size()`.
- Add benchmark wrapper for `BloomAsyncHybridPGMLipp`.
- Register in `CMakeLists.txt` and `benchmark.cc`.
- Add to `scripts/run_final_benchmark.sh`.
- Add to `scripts/analysis_final.py`.
- Build and smoke test with the Facebook lookup-heavy workload.
- Compare against LIPP and current async hybrid.

