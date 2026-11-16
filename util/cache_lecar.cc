// Minimal LeCaR-style cache for LevelDB.
// LRU and LFU recommend victims; ghost hits reduce trust in a bad choice.
#include "leveldb/cache.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

#include "port/port.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb {
namespace {

struct LeCaRHandle {
  std::string key;
  void* value;
  void (*deleter)(const Slice&, void*);
  size_t charge;
  uint32_t hash;
  uint32_t refs;
  uint64_t frequency;
  bool in_cache;
  std::list<LeCaRHandle*>::iterator pos;

  LeCaRHandle(const Slice& k, uint32_t h, void* v, size_t c,
              void (*d)(const Slice&, void*))
      : key(k.data(), k.size()), value(v), deleter(d), charge(c), hash(h),
        refs(1), frequency(1), in_cache(false) {}
};

// A ghost remembers which policy evicted a key, not its value.
struct Ghost {
  bool by_lru;
  std::list<std::string>::iterator pos;
};

class LeCaRCache {
 public:
  LeCaRCache()
      : capacity_(0), usage_(0), lru_weight_(0.5),
        random_state_(1) {}

  ~LeCaRCache() {
    MutexLock l(&mutex_);
    // As in LevelDB, callers must release their handles before destruction.
    for (auto& item : entries_) {
      LeCaRHandle* h = item.second;
      assert(h->refs == 1);
      h->in_cache = false;
      Unref(h);
    }
  }

  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  Cache::Handle* Lookup(const Slice& key, uint32_t hash) {
    MutexLock l(&mutex_);
    const std::string k(key.data(), key.size());
    auto it = entries_.find(k);
    if (it == entries_.end()) return nullptr;

    LeCaRHandle* h = it->second;
    ++h->refs;  // The caller now owns another reference.
    ++h->frequency;
    lru_.splice(lru_.begin(), lru_, h->pos);  // Mark as most recent.
    return reinterpret_cast<Cache::Handle*>(h);
  }

  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge,
                        void (*deleter)(const Slice&, void*)) {
    MutexLock l(&mutex_);
    LeCaRHandle* h = new LeCaRHandle(key, hash, value, charge, deleter);

    // Replacing a value is not an eviction, so do not create a ghost.
    auto old = entries_.find(h->key);
    if (old != entries_.end()) Remove(old->second, false, false);

    // A ghost hit means a previous eviction decision was regrettable.
    auto ghost = ghosts_.find(h->key);
    if (ghost != ghosts_.end()) {
      if (ghost->second.by_lru) {
        lru_weight_ *= 0.9;  // Penalize LRU.
      } else {
        lru_weight_ = 1.0 - (1.0 - lru_weight_) * 0.9;  // Penalize LFU.
      }
      ghost_order_.erase(ghost->second.pos);
      ghosts_.erase(ghost);
    }

    // Oversized entries are returned to callers but not stored.
    if (capacity_ == 0 || charge > capacity_)
      return reinterpret_cast<Cache::Handle*>(h);

    // Make room before insertion. Client-held entries can lose their cache
    // reference; their memory stays alive until the client calls Release().
    while (usage_ > capacity_ - charge && !entries_.empty()) {
      const bool by_lru = Random() < lru_weight_;
      LeCaRHandle* victim = by_lru ? lru_.back() : LFUVictim();
      Remove(victim, true, by_lru);
    }

    ++h->refs;  // The cache also owns a reference.
    h->in_cache = true;
    lru_.push_front(h);
    h->pos = lru_.begin();
    entries_[h->key] = h;
    usage_ += charge;
    return reinterpret_cast<Cache::Handle*>(h);
  }

  void Release(Cache::Handle* handle) {
    MutexLock l(&mutex_);
    Unref(reinterpret_cast<LeCaRHandle*>(handle));
  }

  void Erase(const Slice& key, uint32_t hash) {
    MutexLock l(&mutex_);
    const std::string k(key.data(), key.size());
    auto it = entries_.find(k);
    if (it != entries_.end()) Remove(it->second, false, false);
    auto ghost = ghosts_.find(k);
    if (ghost != ghosts_.end()) {
      ghost_order_.erase(ghost->second.pos);
      ghosts_.erase(ghost);
    }
  }

  void Prune() {
    MutexLock l(&mutex_);
    // Remove only entries without outstanding client references.
    for (auto it = lru_.begin(); it != lru_.end();) {
      LeCaRHandle* h = *it++;
      if (h->refs == 1) Remove(h, false, false);
    }
    ghosts_.clear();
    ghost_order_.clear();
  }

  size_t TotalCharge() const {
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  size_t capacity_;
  mutable port::Mutex mutex_;
  size_t usage_;
  double lru_weight_;  // LFU's probability is 1 - lru_weight_.
  uint32_t random_state_;

  // Front is most recently used; back is least recently used.
  std::list<LeCaRHandle*> lru_;
  std::unordered_map<std::string, LeCaRHandle*> entries_;
  std::list<std::string> ghost_order_;
  std::unordered_map<std::string, Ghost> ghosts_;

  static void Unref(LeCaRHandle* h) {
    assert(h->refs > 0);
    if (--h->refs == 0) {
      h->deleter(Slice(h->key), h->value);
      delete h;
    }
  }

  // Find the least frequently accessed entry; break ties by LRU order.
  LeCaRHandle* LFUVictim() {
    LeCaRHandle* victim = nullptr;
    for (auto it = lru_.rbegin(); it != lru_.rend(); ++it) {
      if (victim == nullptr || (*it)->frequency < victim->frequency)
        victim = *it;
    }
    return victim;
  }

  // Small deterministic pseudorandom generator for policy selection.
  double Random() {
    random_state_ = random_state_ * 1664525u + 1013904223u;
    return static_cast<double>(random_state_) / 4294967296.0;
  }

  // Remove a resident. Only capacity evictions create learning history.
  void Remove(LeCaRHandle* h, bool remember, bool by_lru) {
    if (remember) {
      auto old = ghosts_.find(h->key);
      if (old != ghosts_.end()) {
        ghost_order_.erase(old->second.pos);
        ghosts_.erase(old);
      }
      ghost_order_.push_front(h->key);
      ghosts_[h->key] = {by_lru, ghost_order_.begin()};
      // Bound ghost history to the number of cached entries (at least one).
      // This simple bound is based on entry count, not charge.
      const size_t limit = std::max<size_t>(1, entries_.size());
      while (ghosts_.size() > limit) {
        ghosts_.erase(ghost_order_.back());
        ghost_order_.pop_back();
      }
    }
    entries_.erase(h->key);
    lru_.erase(h->pos);
    usage_ -= h->charge;
    h->in_cache = false;
    Unref(h);  // Drop cache ownership; outstanding handles remain valid.
  }
};

static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;

class ShardedLeCaRCache : public Cache {
 public:
  explicit ShardedLeCaRCache(size_t capacity) : last_id_(0) {
    const size_t per_shard = (capacity + kNumShards - 1) / kNumShards;
    for (int i = 0; i < kNumShards; ++i) shard_[i].SetCapacity(per_shard);
  }

  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice&, void*)) override {
    uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }
  Handle* Lookup(const Slice& key) override {
    uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  void Release(Handle* handle) override {
    LeCaRHandle* h = reinterpret_cast<LeCaRHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }
  void Erase(const Slice& key) override {
    uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }
  void* Value(Handle* handle) override {
    return reinterpret_cast<LeCaRHandle*>(handle)->value;
  }
  uint64_t NewId() override {
    MutexLock l(&id_mutex_);
    return ++last_id_;
  }
  void Prune() override {
    for (int i = 0; i < kNumShards; ++i) shard_[i].Prune();
  }
  size_t TotalCharge() const override {
    size_t total = 0;
    for (int i = 0; i < kNumShards; ++i) total += shard_[i].TotalCharge();
    return total;
  }

 private:
  static uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }
  static uint32_t Shard(uint32_t hash) {
    return hash >> (32 - kNumShardBits);
  }
  LeCaRCache shard_[kNumShards];
  port::Mutex id_mutex_;
  uint64_t last_id_;
};

}  // namespace

Cache* NewLeCaRCache(size_t capacity) {
  return new ShardedLeCaRCache(capacity);
}

}  // namespace leveldb
