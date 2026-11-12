// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <algorithm>
#include <cassert>
#include <atomic>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

#include "util/hash.h"
#include "port/port.h"
#include "util/mutexlock.h"
#include "port/thread_annotations.h"

namespace leveldb {
// ARC cache implementation

// Combination of LRU and LFU 
// Cache has 4 LRUs, where 2 of them (T1 and T2) are a actual caches and 2 of them (B1 and B2) are ghosts 
// (aka the ones kicked from T1 and T2). If there is a hit in B1, it means it needs room for more recently 
// accessed items so increase p_ to increase T1 size. If there is a hit in B2, it means it needs room for more
// frequently accessed items so decrease p_ to increase T2 size. 

// T1: entries seen once [actual cache]
// T2: entries seen > once (revived from ghost) [actual cache]
// B1: keys evicted from T1 [not actual cache - only keys]
// B2: keys evicted from T2 [not actual cache]
// p_ = target size of T1 / capacity - p_ = target size of T2

struct ARCHandle {
  std::string key;
  void* value;
  void (*deleter)(const Slice&, void*);
  size_t charge;
  unsigned refs; 
  enum Queue { kNone, kT1, kT2 } queue;
  std::list<ARCHandle*>::iterator pos;

  ARCHandle(const Slice& k, void* v, size_t c, void (*d)(const Slice&, void*)):
  key(k.data(), k.size()), value(v), deleter(d), charge(c), refs(1), queue(kNone) {}
};

struct Ghost { // for B1 and B2
  size_t charge;
  std::list<std::string>::iterator pos;
};

class ARCCache {
 public:
  ARCCache(size_t capacity)
      : capacity_(capacity), p_(0), usage_(0), t1_charge_(0),
        b1_charge_(0), b2_charge_(0) {}

  ~ARCCache() {
    MutexLock l(&mutex_);
    for (const auto& entry : cache_entries_) assert(entry.second->refs == 1);
    while (!t1_.empty()) RemoveResident(t1_.back(), false);
    while (!t2_.empty()) RemoveResident(t2_.back(), false);
  }

  // Lookup in cache and update position
  Cache::Handle* Lookup(const Slice& key) {
    MutexLock l(&mutex_);
    auto it = cache_entries_.find(std::string(key.data(), key.size()));
    if (it == cache_entries_.end()) return nullptr;
    ARCHandle* h = it->second;
    ++h->refs; 
    if (h->queue == ARCHandle::kT1) {
      t1_.erase(h->pos);
      t1_charge_ -= h->charge;
      t2_.push_front(h);
      h->pos = t2_.begin();
      h->queue = ARCHandle::kT2;
    } else {
      t2_.splice(t2_.begin(), t2_, h->pos);
    }
    return reinterpret_cast<Cache::Handle*>(h);
  }

// Insert into cache and evicts if necessary
  Cache::Handle* Insert(const Slice& key, void* value, size_t charge,
                        void (*deleter)(const Slice&, void*)) {
    ARCHandle* h = new ARCHandle(key, value, charge, deleter);
    MutexLock l(&mutex_);
    auto old = cache_entries_.find(h->key);
    if (old != cache_entries_.end()) RemoveResident(old->second, false);
    ARCHandle::Queue target = ARCHandle::kT1;
    auto g1 = b1_.find(h->key);
    auto g2 = b2_.find(h->key);
    if (g1 != b1_.end()) {
      const size_t ratio = std::max<size_t>(1, b2_charge_ / std::max<size_t>(1, b1_charge_));
      const size_t room = capacity_ - p_;
      const size_t step = ratio > 0 && charge > room / ratio ? room : ratio * charge;
      p_ += step;
      RemoveGhost(b1_, b1_order_, b1_charge_, g1);
      target = ARCHandle::kT2;
    } else if (g2 != b2_.end()) {
      const size_t ratio = std::max<size_t>(1, b1_charge_ / std::max<size_t>(1, b2_charge_));
      const size_t step = ratio > 0 && charge > p_ / ratio ? p_ : ratio * charge;
      p_ -= step;
      RemoveGhost(b2_, b2_order_, b2_charge_, g2);
      target = ARCHandle::kT2;
    }

    if (capacity_ == 0 || charge > capacity_) return reinterpret_cast<Cache::Handle*>(h);

    while (usage_ > capacity_ - charge && Replace(target == ARCHandle::kT2)) {}
    ++h->refs; 
    cache_entries_[h->key] = h;
    if (target == ARCHandle::kT1) {
      t1_.push_front(h);
      h->pos = t1_.begin();
      t1_charge_ += charge;
    } else {
      t2_.push_front(h);
      h->pos = t2_.begin();
    }
    h->queue = target;
    usage_ += charge;
    TrimGhosts();
    return reinterpret_cast<Cache::Handle*>(h);
  }

  // Decrease an entry's reference count and deletes if no one owns
  void Release(Cache::Handle* handle) {
    MutexLock l(&mutex_);
    Unref(reinterpret_cast<ARCHandle*>(handle));
    while (usage_ > capacity_ && Replace(false)) {}
  }

  // Remove key from cache
  void Erase(const Slice& key) {
    MutexLock l(&mutex_);
    const std::string k(key.data(), key.size());
    auto it = cache_entries_.find(k);
    if (it != cache_entries_.end()) RemoveResident(it->second, false);
    auto g1 = b1_.find(k);
    if (g1 != b1_.end()) RemoveGhost(b1_, b1_order_, b1_charge_, g1);
    auto g2 = b2_.find(k);
    if (g2 != b2_.end()) RemoveGhost(b2_, b2_order_, b2_charge_, g2);
  }

  // Remove cached entries if no references aka no client holding it
  void Prune() {
    MutexLock l(&mutex_);
    for (auto it = t1_.begin(); it != t1_.end();) {
      ARCHandle* h = *it++;
      if (h->refs == 1) RemoveResident(h, false);
    }
    for (auto it = t2_.begin(); it != t2_.end();) {
      ARCHandle* h = *it++;
      if (h->refs == 1) RemoveResident(h, false);
    }
    b1_.clear();
    b2_.clear();
    b1_order_.clear();
    b2_order_.clear();
    b1_charge_ = b2_charge_ = 0;
  }

  size_t TotalCharge() const {
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  using GhostMap = std::unordered_map<std::string, Ghost>;
  using GhostIt = GhostMap::iterator;
  const size_t capacity_;
  size_t p_, usage_, t1_charge_, b1_charge_, b2_charge_;
  std::list<ARCHandle*> t1_, t2_;
  std::list<std::string> b1_order_, b2_order_;
  std::unordered_map<std::string, ARCHandle*> cache_entries_;
  GhostMap b1_, b2_;
  mutable port::Mutex mutex_;

  static void Unref(ARCHandle* h) {
    assert(h->refs > 0);
    if (--h->refs == 0) {
      h->deleter(Slice(h->key), h->value);
      delete h;
    }
  }

  // Delete ghost from B1/B2 so it can get added to T1/T2
  void RemoveGhost(GhostMap& map, std::list<std::string>& order, size_t& bytes, GhostIt it) {
    bytes -= it->second.charge;
    order.erase(it->second.pos);
    map.erase(it);
  }

  // Remove old ghosts for capacity LRU
  void TrimGhosts() {
    while (b1_charge_ + b2_charge_ > capacity_) {
      const bool from_b1 = !b1_order_.empty() &&
                           (b2_order_.empty() || b1_charge_ >= b2_charge_);
      if (from_b1) {
        RemoveGhost(b1_, b1_order_, b1_charge_, b1_.find(b1_order_.back()));
      } else {
        RemoveGhost(b2_, b2_order_, b2_charge_, b2_.find(b2_order_.back()));
      }
    }
  }

  // Add ghost to B1 if from T1, else add ghost to B2
  void AddGhost(const std::string& key, size_t charge, bool from_t1) {
    GhostMap& map = from_t1 ? b1_ : b2_;
    std::list<std::string>& order = from_t1 ? b1_order_ : b2_order_;
    size_t& bytes = from_t1 ? b1_charge_ : b2_charge_;
    auto old = map.find(key);
    if (old != map.end()) {
        RemoveGhost(map, order, bytes, old);
    }
    order.push_front(key);
    map.emplace(key, Ghost{charge, order.begin()});
    bytes += charge;
    TrimGhosts();
  }

  // Remove from T1/T2 and move it into ghost if it is getting removed bc of capacity
  void RemoveResident(ARCHandle* h, bool make_ghost) {
    assert(h->queue != ARCHandle::kNone);
    const bool from_t1 = h->queue == ARCHandle::kT1;
    if (from_t1) {
      t1_.erase(h->pos);
      t1_charge_ -= h->charge;
    } else {
      t2_.erase(h->pos);
    }
    usage_ -= h->charge;
    cache_entries_.erase(h->key);
    h->queue = ARCHandle::kNone;
    if (make_ghost) AddGhost(h->key, h->charge, from_t1);
    Unref(h);
  }

  // Find LRU entry that isn't being referenced by other clients to get deleted
  static ARCHandle* Victim(std::list<ARCHandle*>& entries) {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      if ((*it)->refs == 1) return *it;
    }
    return nullptr;
  }

  // Pick whether to evict from T1/T2 based on p_
  bool Replace(bool incoming_from_b2) {
    const bool prefer_t1 = t1_charge_ > p_ || (incoming_from_b2 && t1_charge_ == p_);
    ARCHandle* victim = prefer_t1 ? Victim(t1_) : Victim(t2_);
    if (victim == nullptr) victim = prefer_t1 ? Victim(t2_) : Victim(t1_);
    if (victim == nullptr) return false; 
    RemoveResident(victim, true);
    return true;
  }
};

// ShardedARC
class ShardedARCCache : public Cache {
 private:
  std::atomic<uint64_t> next_id_;
  static constexpr int kNumShards = 16;
  ARCCache* shards_[kNumShards];

  static unsigned Shard(const Slice& key) { return Hash(key.data(), key.size(), 0) >> 28; }
  
 public:
  explicit ShardedARCCache(size_t capacity) : next_id_(0) {
    const size_t per_shard = (capacity + kNumShards - 1) / kNumShards;
    for (int i = 0; i < kNumShards; ++i) {
      shards_[i] = new ARCCache(per_shard);
    }
  }
  ~ShardedARCCache() override {
    for (auto* shard : shards_) delete shard;
  }
  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice&, void*)) override {
    return shards_[Shard(key)]->Insert(key, value, charge, deleter);
  }
  Handle* Lookup(const Slice& key) override {
    return shards_[Shard(key)]->Lookup(key);
  }
  void Release(Handle* handle) override {
    ARCHandle* h = reinterpret_cast<ARCHandle*>(handle);
    shards_[Shard(Slice(h->key))]->Release(handle);
  }
  void* Value(Handle* handle) override {
    return reinterpret_cast<ARCHandle*>(handle)->value;
  }
  void Erase(const Slice& key) override { shards_[Shard(key)]->Erase(key); }
  uint64_t NewId() override { return ++next_id_; }
  void Prune() override {
    for (auto* shard : shards_) shard->Prune();
  }
  size_t TotalCharge() const override {
    size_t total = 0;
    for (const auto* shard : shards_) total += shard->TotalCharge();
    return total;
  }
};

Cache* NewARCCache(size_t capacity) { return new ShardedARCCache(capacity); }

}  // namespace leveldb
