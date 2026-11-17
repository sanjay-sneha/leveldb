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

// LeCaR inspired implementation

// LRU stores cached entries, each with a frequency counter (used by LFU)
// LRU and LFU recommend eviction victims
// Ghost hits reduce trust in that type of cache
// Find optimal ratio between the two

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

  LeCaRHandle(const Slice& k, uint32_t h, void* v, size_t c, void (*d)(const Slice&, void*))
    : key(k.data(), k.size()), value(v), deleter(d), charge(c), hash(h), refs(1), frequency(1), in_cache(false) {}
};

// Stores evicted key and which cache type evicted it: LRU or not -> if a ghost gets hit, its cache type gets penalized
struct Ghost {
  bool by_lru;
  std::list<std::string>::iterator pos;
};

class LeCaRCache {
 public:
  LeCaRCache() : capacity_(0), usage_(0), lru_weight_(0.5), random_(1) {}

  ~LeCaRCache() {
    MutexLock l(&mutex_);
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
    ++h->refs; 
    ++h->frequency; // Count the number of times this entry has been seen for LFU
    lru_.splice(lru_.begin(), lru_, h->pos); // Store in LRU
    return reinterpret_cast<Cache::Handle*>(h);
  }

  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value, size_t charge, void (*deleter)(const Slice&, void*)) {
    MutexLock l(&mutex_);
    LeCaRHandle* h = new LeCaRHandle(key, hash, value, charge, deleter);
    auto old = entries_.find(h->key);
    if (old != entries_.end()) Remove(old->second, false, false);

    // A ghost hit means the cooresponding cache strategy wasn't a good fit
    auto ghost = ghosts_.find(h->key);
    if (ghost != ghosts_.end()) {
      if (ghost->second.by_lru) {
        lru_weight_ *= 0.9;  // Penalize LRU
      } else {
        lru_weight_ = 1.0 - (1.0 - lru_weight_) * 0.9;  // Penalize LFU
      }
      ghost_order_.erase(ghost->second.pos);
      ghosts_.erase(ghost);
    }
    if (capacity_ == 0 || charge > capacity_)
      return reinterpret_cast<Cache::Handle*>(h);

    // Make room to insert
    while (usage_ > capacity_ - charge && !entries_.empty()) {
      const bool by_lru = Random() < lru_weight_; // Pick if LRU or LFU
      LeCaRHandle* victim = by_lru ? lru_.back() : LFUVictim();
      Remove(victim, true, by_lru);
    }
    ++h->refs;  
    h->in_cache = true;
    lru_.push_front(h);
    h->pos = lru_.begin();
    entries_[h->key] = h;
    usage_ += charge;
    return reinterpret_cast<Cache::Handle*>(h);
  }

  // Decrease an entry's reference count and deletes if no one owns
  void Release(Cache::Handle* handle) {
    MutexLock l(&mutex_);
    Unref(reinterpret_cast<LeCaRHandle*>(handle));
  }

  // Remove key from cache
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


  // Remove cached entries if no references aka no client holding it
  void Prune() {
    MutexLock l(&mutex_);
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
  double lru_weight_;  // LFU = 1 - lru_weight_
  uint32_t random_;

  
  std::list<LeCaRHandle*> lru_; // Most recently used to least recently used 
  std::unordered_map<std::string, LeCaRHandle*> entries_; // Fast entries lookup 
  std::list<std::string> ghost_order_; // To get rid of the oldest ghost so bounded ghosts
  std::unordered_map<std::string, Ghost> ghosts_; // Fast ghost lookup and easy ghost removal

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

  // Pseudorandom generator
  double Random() {
    random_ = random_ * 1664525u + 1013904223u;
    return static_cast<double>(random_) / 4294967296.0;
  }

  // Remove an entry if capacity kicked it
  void Remove(LeCaRHandle* h, bool remember, bool by_lru) {
    if (remember) {
      auto old = ghosts_.find(h->key);
      if (old != ghosts_.end()) {
        ghost_order_.erase(old->second.pos);
        ghosts_.erase(old);
      }
      ghost_order_.push_front(h->key);
      ghosts_[h->key] = {by_lru, ghost_order_.begin()};
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
    Unref(h);  
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

  Handle* Insert(const Slice& key, void* value, size_t charge, void (*deleter)(const Slice&, void*)) override {
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

}  

Cache* NewLeCaRCache(size_t capacity) {
  return new ShardedLeCaRCache(capacity);
}

}  // namespace leveldb
