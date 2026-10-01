// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb {

Cache::~Cache() {}

namespace {
enum class FromQueue {A1In, A1Out, Am};

// Updated 2Q Cache Version -> built on top of original LRU implementation
//
// Cache entries have an "in_cache" boolean indicating whether the cache has a
// reference on the entry.  The only ways that this can become false without the
// entry being passed to its "deleter" are via Erase(), via Insert() when
// an element with a duplicate key is inserted, or on destruction of the cache.
//
// The cache keeps 4 linked lists. The lists are:
// - in-use:  contains the items currently referenced by clients, in no
//   particular order.  (This list is used for invariant checking.  If we
//   removed the check, elements that would otherwise be on this list could be
//   left as disconnected singleton lists.) [Same as LevelDB]
// - A1In: In memory first-access buffer, FIFO queue
// - A1Out: FIFO queue that stores the keys of recently evicted keys from A1In, if spotted again, gets moved into Am
// - Am: The actual cache (LRU) that holds entries accessed more than once
// Elements are moved between these lists by the Ref() and Unref() methods,
// when they detect an element in the cache acquiring or losing its only
// external reference.

// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.
struct TwoQueueHandle {
  void* value;
  void (*deleter)(const Slice&, void* value);
  TwoQueueHandle* next_hash;
  TwoQueueHandle* next;
  TwoQueueHandle* prev;
  size_t charge;  // TODO(opt): Only allow uint32_t?
  size_t key_length;
  bool in_cache;     // Whether entry is in the cache.
  uint32_t refs;     // References, including cache reference, if present.
  uint32_t hash;     // Hash of key(); used for fast sharding and comparisons
  char key_data[1];  // Beginning of key
  FromQueue from;   // ADDED: which queue should this node go back to if moved to in_use (A1In, Am)

  Slice key() const {
    // next is only equal to this if the LRU handle is the list head of an
    // empty list. List heads never have meaningful keys.
    assert(next != this);

    return Slice(key_data, key_length);
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
class TwoQueueHandleTable {
 public:
  TwoQueueHandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
  ~TwoQueueHandleTable() { delete[] list_; }

  TwoQueueHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }

  TwoQueueHandle* Insert(TwoQueueHandle* h) {
    TwoQueueHandle** ptr = FindPointer(h->key(), h->hash);
    TwoQueueHandle* old = *ptr;
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;
    if (old == nullptr) {
      ++elems_;
      if (elems_ > length_) {
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        Resize();
      }
    }
    return old;
  }

  TwoQueueHandle* Remove(const Slice& key, uint32_t hash) {
    TwoQueueHandle** ptr = FindPointer(key, hash);
    TwoQueueHandle* result = *ptr;
    if (result != nullptr) {
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }

 private:
  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  uint32_t length_;
  uint32_t elems_;
  TwoQueueHandle** list_;

  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  TwoQueueHandle** FindPointer(const Slice& key, uint32_t hash) {
    TwoQueueHandle** ptr = &list_[hash & (length_ - 1)];
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }

  void Resize() {
    uint32_t new_length = 4;
    while (new_length < elems_) {
      new_length *= 2;
    }
    TwoQueueHandle** new_list = new TwoQueueHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    for (uint32_t i = 0; i < length_; i++) {
      TwoQueueHandle* h = list_[i];
      while (h != nullptr) {
        TwoQueueHandle* next = h->next_hash;
        uint32_t hash = h->hash;
        TwoQueueHandle** ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
};

// A single shard of sharded cache.
class TwoQueueCache {
 public:
  TwoQueueCache();
  ~TwoQueueCache();

  // Separate from constructor so caller can easily make an array of LRUCache
  void SetCapacity(size_t capacity) {  
    capacity_ = capacity; 
    kin_capacity_ = capacity / 4;    //ADDED kin & kout
    kout_capacity_ = capacity / 2;
  }

  // Like Cache methods, but with an extra "hash" parameter.
  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge,
                        void (*deleter)(const Slice& key, void* value));
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
  void Prune();
  size_t TotalCharge() const {
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  void List_Remove(TwoQueueHandle* e);
  void List_Append(TwoQueueHandle* list, TwoQueueHandle* e);
  void Ref(TwoQueueHandle* e);
  void Unref(TwoQueueHandle* e);
  void RemoveE(TwoQueueHandle* e);
  void ToA1Out(TwoQueueHandle* e);
  void EvictAm(TwoQueueHandle* e);
  void EvictA1Out(TwoQueueHandle* e);
  TwoQueueHandle* EvictA1In();
  void Evict();
  void Free_List(TwoQueueHandle* head);
  void Free_Out_List(TwoQueueHandle* head);
  
  // Initialized before use.
  size_t capacity_;
  size_t kin_capacity_;
  size_t kout_capacity_;
  size_t a1in_charge_;
  size_t a1out_charge_;

  // mutex_ protects the following state.
  mutable port::Mutex mutex_;
  size_t usage_ GUARDED_BY(mutex_);


  // ADDED: new lists instead of LRU
  // A1in FIFO first wave of seen
  TwoQueueHandle a1in_ GUARDED_BY(mutex_);
  //A1Out FIFO graveyard, if seen again -> Am
  TwoQueueHandle a1out_ GUARDED_BY(mutex_);
  // Am LRU cache of entries that appear more than once
  TwoQueueHandle am_ GUARDED_BY(mutex_);

  // Dummy head of in-use list.
  // Entries are in use by clients, and have refs >= 2 and in_cache==true.
  TwoQueueHandle in_use_ GUARDED_BY(mutex_);

  TwoQueueHandleTable table_ GUARDED_BY(mutex_);
};

// ADDED k_in & k_out
TwoQueueCache::TwoQueueCache() : capacity_(0), usage_(0), kin_capacity_(0), kout_capacity_(0), a1in_charge_(0), a1out_charge_(0) {
  // Make empty circular linked lists.
  a1in_.next = a1in_.prev = &a1in_;
  a1out_.next = a1out_.prev = &a1out_;
  am_.next = am_.prev = &am_;
  in_use_.next = &in_use_;
  in_use_.prev = &in_use_;
}

TwoQueueCache::~TwoQueueCache() {
  assert(in_use_.next == &in_use_);  // Error if caller has an unreleased handle
  Free_List(&a1in_); // ADDED: to free the newly added lists
  Free_List(&am_);
  Free_Out_List(&a1out_);
}

void TwoQueueCache::Ref(TwoQueueHandle* e) {
  assert(e->refs > 0);
  if (e->refs == 1 && e->in_cache) {  
    if (e->from == FromQueue::Am) { // ADDED: Am is the LRU, thus we only move the entry for Am
      List_Remove(e);                // ADDED: we don't do A1In bc that would break FIFO
      List_Append(&in_use_, e);
    }
    
  }
  e->refs++;
}

void TwoQueueCache::Unref(TwoQueueHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs == 0) {  // Deallocate.
    assert(!e->in_cache);
    (*e->deleter)(e->key(), e->value);
    free(e);
  } else if (e->in_cache && e->refs == 1 && e->from == FromQueue::Am) { //ADDED: Am check
    List_Remove(e);
    List_Append(&am_, e);
  }
}

void TwoQueueCache::List_Remove(TwoQueueHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

void TwoQueueCache::List_Append(TwoQueueHandle* list, TwoQueueHandle* e) {
  // Make "e" newest entry by inserting just before *list
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}

Cache::Handle* TwoQueueCache::Lookup(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  TwoQueueHandle* e = table_.Lookup(key, hash);
  if (e != nullptr && e->from != FromQueue::A1Out) { // ADDED: A1Out check
    Ref(e);
  }
  return reinterpret_cast<Cache::Handle*>(e);
}

void TwoQueueCache::Release(Cache::Handle* handle) {
  MutexLock l(&mutex_);
  Unref(reinterpret_cast<TwoQueueHandle*>(handle));
}

//ADDED: changed logic for 2q so mostly rewritten
Cache::Handle* TwoQueueCache::Insert(const Slice& key, uint32_t hash, void* value,
                                size_t charge,
                                void (*deleter)(const Slice& key,
                                                void* value)) {
  MutexLock l(&mutex_);

  TwoQueueHandle* existing = table_.Lookup(key, hash);
  // Ghost hit -> should be moved into Am
  if (existing != nullptr && existing->from == FromQueue::A1Out) {
    assert(existing->in_cache);
    assert(existing->refs == 1);
    assert(existing->value == nullptr);
    List_Remove(existing);
    a1out_charge_ -= existing->charge;
    existing->value = value;
    existing->deleter = deleter;
    existing->charge = charge;
    existing->from = FromQueue::Am;
    existing->refs = 2;
    List_Append(&in_use_, existing);
    usage_ += charge;
    Evict();
    return reinterpret_cast<Cache::Handle*>(existing);
  }
  // Duplicate -> remove the old instance
  if (existing != nullptr) {
    TwoQueueHandle* removed = table_.Remove(key, hash);
    RemoveE(existing);
  }
  // Else first time spotted -> add into A1In
  TwoQueueHandle* e = reinterpret_cast<TwoQueueHandle*>(malloc(sizeof(TwoQueueHandle) - 1 + key.size()));
  e->value = value;
  e->deleter = deleter;
  e->next_hash = nullptr;
  e->next = nullptr;
  e->prev = nullptr;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->in_cache = true;
  e->refs = 2;  // for the returned handle.
  e->from = FromQueue::A1In;
  std::memcpy(e->key_data, key.data(), key.size());

  List_Append(&a1in_, e);
  a1in_charge_ += charge;
  usage_ += charge;
  
  table_.Insert(e);
  Evict(); // in case too full

  return reinterpret_cast<Cache::Handle*>(e);
}

// ADDED: changed for 2q
void TwoQueueCache::Erase(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  TwoQueueHandle* e = table_.Remove(key, hash);
  if (e == nullptr) {
    return;
  }
  if (e->from == FromQueue::A1Out) {
    List_Remove(e);
    a1out_charge_ -= e->charge;
    e->in_cache = false;
    free(e);
    return;
  }
  RemoveE(e);
}

// HELPERS
//ADDED: helper function to remove entry from cache
void TwoQueueCache::RemoveE(TwoQueueHandle* e) {
  List_Remove(e);
  if (e->from == FromQueue::A1In) {
    a1in_charge_ -= e->charge;
  }
  usage_ -= e->charge;
  e->in_cache = false;
  Unref(e);
}

// ADDED: helper function to move entry from A1In to A1Out
void TwoQueueCache::ToA1Out(TwoQueueHandle* e){
  List_Remove(e);
  a1in_charge_ -= e->charge;
  usage_ -= e->charge;
  (*e->deleter)(e->key(), e->value);
  e->value = nullptr;
  e->deleter = nullptr;
  e->from = FromQueue::A1Out;
  a1out_charge_ += e->charge;
  List_Append(&a1out_, e);
}

// ADDED: Evict Am LRU
void TwoQueueCache::EvictAm(TwoQueueHandle* e) {
  TwoQueueHandle* removed = table_.Remove(e->key(), e->hash);
  List_Remove(e);
  usage_ -= e->charge;
  e->in_cache = false;
  Unref(e);
}

// ADDED: Evict A1Out FIFO
void TwoQueueCache::EvictA1Out(TwoQueueHandle* e) {
  TwoQueueHandle* removed = table_.Remove(e->key(), e->hash);
  List_Remove(e);
  a1out_charge_ -= e->charge;
  e->in_cache = false;
  free(e);
}

// ADDED: Evict A1In and return oldest val to move into A1Out
TwoQueueHandle* TwoQueueCache::EvictA1In() {
  TwoQueueHandle* e = a1in_.next;
  while (e != &a1in_) {
    if (e->refs == 1) {
      return e;
    }
    e = e->next;
  }
  return nullptr;
}

// ADDED: Evict logic
void TwoQueueCache::Evict(){
  // if exceed capacity
  while (usage_ > capacity_) {
    bool evicted = false;
    // if we are past kin, aka desired capacity of the queue, we push oldest into A1Out to prevent overflow
    if (a1in_charge_ > kin_capacity_) {
      TwoQueueHandle* oldest = EvictA1In();
      if (oldest != nullptr) {
        ToA1Out(oldest);
        evicted = true;
      }
    }
    if (evicted) {
      continue;
    }
    // otherwise, evict LRU
    if (am_.next != &am_) {
      EvictAm(am_.next);
      continue;
    }

    // worst case go back to trying to empty A1in
    TwoQueueHandle* oldest = EvictA1In();
    if (oldest != nullptr) {
      ToA1Out(oldest);
      continue;
    }
    break;
  }
  // if a1out size is too much and there is something in a1out, evict oldest
  while (a1out_charge_ > kout_capacity_ && a1out_.next != &a1out_) {
    EvictA1Out(a1out_.next);
  }
}

void TwoQueueCache::Free_List(TwoQueueHandle* head){
  for (TwoQueueHandle* e = head->next; e!= head;) {
    TwoQueueHandle* next = e->next;
    e->in_cache = false;
    (*e->deleter)(e->key(), e->value);
    free(e);
    e = next;
  }

}

void TwoQueueCache::Free_Out_List(TwoQueueHandle* head){
  for (TwoQueueHandle* e = head->next; e != head;) {
    TwoQueueHandle* next = e->next;
    e->in_cache = false;
    free(e);
    e = next;
  }
}

static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;

// ADDED: altered names for two queue
class ShardedTwoQueueCache : public Cache {
 private:
  TwoQueueCache shard_[kNumShards];
  port::Mutex id_mutex_;
  uint64_t last_id_;

  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

 public:
  explicit ShardedTwoQueueCache(size_t capacity) : last_id_(0) {
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].SetCapacity(per_shard);
    }
  }
  ~ShardedTwoQueueCache() override {}
  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice& key, void* value)) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }
  Handle* Lookup(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  void Release(Handle* handle) override {
    TwoQueueHandle* h = reinterpret_cast<TwoQueueHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }
  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }
  void* Value(Handle* handle) override {
    return reinterpret_cast<TwoQueueHandle*>(handle)->value;
  }
  uint64_t NewId() override {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }
  void Prune() override {
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].Prune();
    }
  }
  size_t TotalCharge() const override {
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shard_[s].TotalCharge();
    }
    return total;
  }
};

}  // end anonymous namespace

Cache* NewLRUCache(size_t capacity) { return new ShardedTwoQueueCache(capacity); }

}  // namespace leveldb
