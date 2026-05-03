# Task 2 - Milestone 3 Plan: Asynchronous Hybrid DPGM + LIPP

**Goal:** improve the Milestone 2 naive hybrid by replacing synchronous foreground
flushes with an asynchronous flush strategy. The final benchmark still compares
DynamicPGM, LIPP, and the hybrid on the two mixed workloads across all datasets.

---

## 1. Motivation

Milestone 2 uses a naive synchronous hybrid:

- LIPP stores the bulk-loaded initial data.
- DPGM receives new inserts.
- Once DPGM reaches a threshold, the foreground operation calls `Flush()`.
- `Flush()` iterates the entire DPGM and inserts every key into LIPP one by one.

This is simple and correct, but it creates a large foreground pause. One insert
operation can pay the full migration cost, which hurts mixed-workload throughput.

Milestone 3 should move that migration work to a background thread. The foreground
operation stream stays single-threaded, but the async flush creates one extra
concurrent actor: the background flush worker.

The main thread-safety problem is therefore:

```text
foreground lookup may read LIPP while background flush worker writes LIPP
```

The design below uses a double-buffered DPGM plus explicit LIPP locking to keep
the no-keys-left-behind invariant.

---

## 2. High-Level Design

Use two DPGM roles:

1. **Active DPGM**
   - Receives all new foreground inserts.
   - Is searched first by foreground lookups.
   - Is owned only by the foreground benchmark thread.

2. **Snapshot DPGM**
   - Created when active DPGM reaches the flush threshold.
   - Is immutable after creation.
   - Is searched by lookups while the background worker migrates it into LIPP.
   - Is retired only after every snapshot key has been inserted into LIPP.

LIPP remains the main lookup-optimized store:

- Foreground lookups read LIPP after checking active DPGM and snapshot DPGM.
- The background worker writes to LIPP during migration.
- LIPP therefore needs read/write synchronization.

Conceptual flow:

```text
Initial state:
  LIPP = bulk-loaded keys
  active_dpgm = empty
  snapshot_dpgm = none

Foreground insert:
  insert into active_dpgm
  if active_dpgm reaches threshold:
    publish active_dpgm as immutable snapshot_dpgm
    reset active_dpgm to empty
    wake background flush worker

During async flush:
  foreground inserts -> active_dpgm
  foreground lookups -> active_dpgm, then snapshot_dpgm, then LIPP
  flush worker -> iterates snapshot_dpgm and inserts batches into LIPP

After async flush:
  snapshot_dpgm is retired
  foreground lookups -> active_dpgm, then LIPP
```

---

## 3. Ownership And Thread-Safety Model

Assume the benchmark workload remains single-threaded. There is still concurrency
because the hybrid starts a background flush worker.

### 3.1 Active DPGM

`active_dpgm_` is accessed only by the foreground thread:

- `Insert()` mutates it.
- `EqualityLookup()` reads it.
- The background worker never reads or writes it.

Therefore, `active_dpgm_` does **not** need a per-operation lock.

The only synchronized moment is the threshold handoff:

```text
foreground thread:
  move active_dpgm_ into snapshot_dpgm_
  replace active_dpgm_ with an empty DPGM
  wake flush worker
```

After this handoff, the flush worker owns only the snapshot. It must never touch
the new active DPGM.

### 3.2 Snapshot DPGM

`snapshot_dpgm_` is immutable once published:

- The flush worker iterates it.
- Foreground lookups may search it.
- No thread inserts into it.

Because it is immutable, concurrent reads are safe as long as the object lifetime
is safe.

Use `std::shared_ptr<const DPGMType>` for `snapshot_dpgm_`:

- Lookup copies the shared pointer under a small mutex.
- The mutex is released before searching the snapshot.
- The local `shared_ptr` keeps the snapshot alive even if the worker retires the
  global pointer after the flush completes.

This lock protects the pointer/lifetime, not the DPGM internals.

### 3.3 LIPP

LIPP is not naturally safe for concurrent read/write.

`find()` walks raw node pointers, bitmaps, and child pointers. `insert()` may
change root, rebuild nodes, update bitmaps, replace child pointers, and mutate
the tree structure.

Therefore:

- `lipp_.find()` must hold a shared/read lock.
- `lipp_.insert()` must hold an exclusive/write lock.

Use `std::shared_mutex`:

```cpp
mutable std::shared_mutex lipp_mutex_;
```

Lookup uses:

```cpp
std::shared_lock<std::shared_mutex> lock(lipp_mutex_);
lipp_.find(key, value);
```

Flush worker uses:

```cpp
std::unique_lock<std::shared_mutex> lock(lipp_mutex_);
lipp_.insert(key, value);
```

The write lock must not be held for the entire snapshot flush. Instead, insert
snapshot keys into LIPP in small batches.

---

## 4. Class State To Add

Modify `competitors/hybrid_pgm_lipp.h`.

The current single DPGM member should become active plus snapshot state:

```cpp
DPGMType active_dpgm_;
size_t active_count_ = 0;

std::shared_ptr<const DPGMType> snapshot_dpgm_;
std::thread flush_thread_;

mutable std::mutex snapshot_mutex_;
std::condition_variable flush_cv_;

mutable std::shared_mutex lipp_mutex_;

bool flush_requested_ = false;
bool flush_in_progress_ = false;
bool shutdown_ = false;

static constexpr size_t kFlushBatchSize = 1024;
```

Required headers:

```cpp
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
```

Keep existing members:

```cpp
LIPP<KeyType, uint64_t> lipp_;
size_t initial_count_ = 0;
size_t flush_threshold_ = 0;
```

---

## 5. Build Path

`Build()` should still bulk-load initial data into LIPP. No LIPP lock is needed
during `Build()` because foreground operations have not started yet.

After computing `flush_threshold_`, start the background worker:

```cpp
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

  shutdown_ = false;
  flush_thread_ = std::thread(&HybridPGMLipp::FlushWorker, this);

  return build_time;
}
```

---

## 6. Destructor

Add a destructor so the background worker is always joined.

Recommended behavior: let any in-progress snapshot flush finish before the object
is destroyed. This preserves correctness and makes the reported final index size
more meaningful.

```cpp
~HybridPGMLipp() {
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    shutdown_ = true;
    flush_cv_.notify_one();
  }

  if (flush_thread_.joinable()) {
    flush_thread_.join();
  }
}
```

The worker loop should interpret shutdown carefully:

- If `shutdown_ == true` and there is no pending snapshot, exit.
- If `shutdown_ == true` but a snapshot is pending or in progress, finish that
  snapshot first, then exit.

---

## 7. Insert Path

Foreground inserts should remain short.

```cpp
void Insert(const KeyValue<KeyType>& data, uint32_t thread_id) {
  active_dpgm_.insert(data.key, data.value);
  ++active_count_;

  if (active_count_ >= flush_threshold_) {
    MaybeStartFlush();
  }
}
```

`MaybeStartFlush()` performs the active-to-snapshot handoff:

```cpp
void MaybeStartFlush() {
  std::unique_lock<std::mutex> lock(snapshot_mutex_);

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
```

Important details:

- This mutex does not protect every active DPGM read/write.
- It only protects snapshot pointer publication and flush state flags.
- The background worker must never use `active_dpgm_`.

### If Active Fills During An Existing Flush

For the first implementation, keep only one snapshot buffer.

If `active_count_ >= flush_threshold_` while `flush_in_progress_ == true`, do
not create a second snapshot. Just return from `MaybeStartFlush()` and keep
inserting into `active_dpgm_`.

This is safe but may let active DPGM grow beyond the threshold in insert-heavy
workloads. That is acceptable for a first async design because it avoids complex
multi-snapshot lifecycle management.

Possible later improvement:

- Maintain a queue of immutable DPGM snapshots.
- The flush worker drains snapshots in FIFO order.
- Lookups check active DPGM, then every snapshot, then LIPP.

Do not start with this unless the one-snapshot design cannot keep up.

---

## 8. Lookup Path

Lookup must search every place where a key may currently live.

Order:

1. `active_dpgm_`
2. `snapshot_dpgm_`, if present
3. `lipp_`

Pseudo-code:

```cpp
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
    auto sit = snapshot->find(lookup_key);
    if (sit != snapshot->end()) {
      return static_cast<size_t>(sit->value());
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

Lock reasoning:

- Active DPGM: no lock, foreground-only.
- Snapshot DPGM: lock only long enough to copy the `shared_ptr`; no lock during
  snapshot search because it is immutable.
- LIPP: shared lock during `find()` because the background worker may be writing.

---

## 9. Background Flush Worker

The worker waits for a published snapshot, then migrates it to LIPP.

```cpp
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
```

---

## 10. Batched LIPP Insertion

Do not hold the LIPP write lock for the entire snapshot flush:

```text
bad:
  lock LIPP
  insert 500k keys
  unlock LIPP
```

That would block every lookup that reaches LIPP until the full flush completes.

Instead, batch:

```text
better:
  collect 1024 keys
  lock LIPP
  insert 1024 keys
  unlock LIPP
  repeat
```

Pseudo-code:

```cpp
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
```

```cpp
void FlushBatch(const std::vector<std::pair<KeyType, uint64_t>>& batch) {
  std::unique_lock<std::shared_mutex> lock(lipp_mutex_);
  for (const auto& kv : batch) {
    lipp_.insert(kv.first, kv.second);
  }
}
```

Batch size trade-off:

- Smaller batches reduce lookup blocking time but increase lock overhead.
- Larger batches reduce lock overhead but increase lookup stalls.
- Start with `1024`.
- Sweep later over `{256, 1024, 4096, 16384}` if time allows.

---

## 11. Size Reporting

`size()` should include every live structure:

```cpp
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
```

During an active flush, index size may temporarily count duplicated keys:

- Key exists in snapshot DPGM.
- Same key may already have been inserted into LIPP.

This is acceptable and honest because the memory is actually live during the
flush. If the destructor waits for final flush completion before `PrintResult()`,
the reported final size should usually reflect LIPP plus active DPGM, with no
snapshot.

---

## 12. Correctness Invariants

The implementation must preserve:

1. `active_dpgm_` is foreground-owned.
2. `snapshot_dpgm_` is immutable once published.
3. `snapshot_dpgm_` is not retired until every key has been inserted into LIPP.
4. `lipp_.find()` holds a shared lock.
5. `lipp_.insert()` holds an exclusive lock.
6. Lookup checks active DPGM, then snapshot DPGM, then LIPP.
7. The background worker never touches `active_dpgm_`.
8. The destructor joins the background worker.

No-keys-left-behind invariant:

```text
Every inserted key is always reachable from at least one of:
  active_dpgm_
  snapshot_dpgm_
  lipp_
```

---

## 13. Why Thread-Safety Is Critical

LIPP lookup walks mutable raw-pointer state. A simplified version of the relevant
logic is:

```cpp
bool find(const T& key, P& value) const {
  Node* node = root;
  while (true) {
    int pos = PREDICT_POS(node, key);
    if (BITMAP_GET(node->child_bitmap, pos) == 1) {
      node = node->items[pos].comp.child;
    } else {
      if (BITMAP_GET(node->none_bitmap, pos) == 0 &&
          node->items[pos].comp.data.key == key) {
        value = node->items[pos].comp.data.value;
        return true;
      }
      return false;
    }
  }
}
```

LIPP insert can mutate the same structure:

```cpp
void insert(const T& key, const P& value) {
  root = insert_tree(root, key, value);
}
```

Without synchronization, possible failures include:

- Lookup reads a bitmap while insert is modifying it.
- Lookup follows a child pointer while insert replaces it.
- Lookup observes a child pointer before the child node is fully initialized.
- Insert rebuilds a subtree while lookup still holds a pointer into the old one.
- Lookup misses a key that should exist.
- The process crashes due to invalid memory access.

This is why LIPP must be protected with `std::shared_mutex`.

---

## 14. Expected Performance Impact

The synchronous Milestone 2 design has a sawtooth latency pattern:

```text
cheap insert, cheap insert, cheap insert, huge flush pause, cheap insert, ...
```

The async design moves most of the huge flush pause off the foreground path.

Expected benefits:

- Foreground inserts usually remain close to DPGM insert cost.
- Lookup-heavy workloads can continue making progress during migration.
- Active DPGM stays smaller when the flush worker keeps up.
- Flushed keys eventually benefit from LIPP lookup speed.

Expected costs:

- Lookups that reach LIPP may briefly block on batch insert locks.
- Snapshot DPGM adds one extra lookup probe during active flushes.
- Memory usage may temporarily increase because snapshot keys are duplicated in
  snapshot DPGM and LIPP during migration.
- Insert-heavy workloads may outpace the flush worker, causing active DPGM to
  grow beyond threshold.

---

## 15. Implementation Checklist

- [ ] Add required threading headers to `competitors/hybrid_pgm_lipp.h`.
- [ ] Replace `dpgm_` with `active_dpgm_`.
- [ ] Add `snapshot_dpgm_`, worker thread, condition variable, snapshot mutex,
      LIPP shared mutex, and flush state flags.
- [ ] Start flush worker at the end of `Build()`.
- [ ] Add destructor that joins the worker.
- [ ] Change `Insert()` to insert into `active_dpgm_` and call
      `MaybeStartFlush()`.
- [ ] Implement `MaybeStartFlush()` with one-snapshot handoff.
- [ ] Change `EqualityLookup()` to check active DPGM, snapshot DPGM, then LIPP.
- [ ] Guard `lipp_.find()` with shared lock.
- [ ] Implement `FlushWorker()`.
- [ ] Implement `FlushSnapshotToLipp()`.
- [ ] Implement `FlushBatch()` with exclusive LIPP lock.
- [ ] Update `size()` to include active DPGM, snapshot DPGM, and LIPP.
- [ ] Build with `bash scripts/build_benchmark.sh`.
- [ ] Smoke test with one mixed workload and `--only HybridPGMLipp --verify`.
- [ ] Run `bash scripts/run_final_benchmark.sh` for final CSVs.

---

## 16. Possible Extensions

If the one-snapshot design is not fast enough:

1. **Snapshot queue**
   - Allow multiple immutable DPGM snapshots.
   - Worker drains them FIFO.
   - Lookup checks all snapshots before LIPP.

2. **Adaptive threshold**
   - Increase threshold when insert-heavy workload outpaces flush worker.
   - Decrease threshold for lookup-heavy workload to move keys into LIPP sooner.

3. **Batch-size sweep**
   - Sweep `{256, 1024, 4096, 16384}`.
   - Pick the best throughput per dataset/workload.

4. **Optional final drain**
   - Before `size()` or destruction, flush remaining active DPGM synchronously so
     all inserted keys end in LIPP.
   - This may improve final size consistency but adds end-of-run time.
