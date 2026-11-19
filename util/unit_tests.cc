// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "util/coding.h"
#include "util/hash.h"

namespace leveldb {

// factory declarations for all three cache implementations
extern Cache* NewLRUCache(size_t capacity);
extern Cache* NewLFUCache(size_t capacity);
extern Cache* New2QCache(size_t capacity);

// helper conversions between numeric keys/values and slices
static std::string EncodeKey(int k) {
  std::string result;
  PutFixed32(&result, k);
  return result;
}

static int DecodeKey(const Slice& k) {
  assert(k.size() == 4);
  return DecodeFixed32(k.data());
}

static void* EncodeValue(uintptr_t v) { return reinterpret_cast<void*>(v); }
static int DecodeValue(void* v) { return reinterpret_cast<uintptr_t>(v); }

// ADDED: Find keys that compete within one cache shard.
static std::vector<int> KeysInSameShard(size_t count, int first_candidate) {
  std::vector<int> keys;
  uint32_t target_shard = 0;
  for (int key = first_candidate; keys.size() < count; ++key) {
    const std::string encoded = EncodeKey(key);
    const uint32_t hash = Hash(encoded.data(), encoded.size(), 0);
    const uint32_t shard = hash >> 28;
    if (keys.empty()) {
      target_shard = shard;
    }
    if (shard == target_shard) {
      keys.push_back(key);
    }
  }
  return keys;
}

// generic base test class for cache implementations
class CacheTestBase : public testing::Test {
 public:
  static void Deleter(const Slice& key, void* v) {
    current_->deleted_keys_.push_back(DecodeKey(key));
    current_->deleted_values_.push_back(DecodeValue(v));
  }

  static void BenchmarkDeleter(const Slice&, void*) {}

  std::vector<int> deleted_keys_;
  std::vector<int> deleted_values_;
  Cache* cache_ = nullptr;

  ~CacheTestBase() override { delete cache_; }

  int Lookup(int key) {
    Cache::Handle* handle = cache_->Lookup(EncodeKey(key));
    const int r = (handle == nullptr) ? -1 : DecodeValue(cache_->Value(handle));
    if (handle != nullptr) {
      cache_->Release(handle);
    }
    return r;
  }

  void Insert(int key, int value, int charge = 1) {
    cache_->Release(cache_->Insert(EncodeKey(key), EncodeValue(value), charge,
                                   &CacheTestBase::Deleter));
  }

  void InsertForBenchmark(int key, int value, int charge = 1) {
    cache_->Release(cache_->Insert(EncodeKey(key), EncodeValue(value), charge,
                                   &CacheTestBase::BenchmarkDeleter));
  }

  Cache::Handle* InsertAndReturnHandle(int key, int value, int charge = 1) {
    return cache_->Insert(EncodeKey(key), EncodeValue(value), charge,
                          &CacheTestBase::Deleter);
  }

  void Erase(int key) { cache_->Erase(EncodeKey(key)); }

  static CacheTestBase* current_;
};
CacheTestBase* CacheTestBase::current_;

// ============================================================================
// LFU CACHE UNIT TESTS
// ============================================================================

class LFUCacheTest : public CacheTestBase {
 public:
  LFUCacheTest() {
    current_ = this;
    cache_ = NewLFUCache(3 * 16);  // CHANGED: Provide three slots in one shard.
  }
};

// CHANGED: Frequently accessed items survive lower-frequency entries.
TEST_F(LFUCacheTest, FrequencyBasedEviction) {
  const std::vector<int> keys = KeysInSameShard(4, 100);
  Insert(keys[0], 101);  // Initial frequency is 1.
  Insert(keys[1], 201);  // Initial frequency is 1.
  Insert(keys[2], 301);  // Initial frequency is 1.

  // Raise keys[0]'s frequency to 4.
  Lookup(keys[0]);
  Lookup(keys[0]);
  Lookup(keys[0]);

  // Raise keys[1]'s frequency to 2.
  Lookup(keys[1]);

  // keys[2] remains at frequency 1.

  // Inserting keys[3] evicts the lowest-frequency entry.
  Insert(keys[3], 401);

  ASSERT_EQ(101, Lookup(keys[0]));  // Frequency 4 is retained.
  ASSERT_EQ(201, Lookup(keys[1]));  // Frequency 2 is retained.
  ASSERT_EQ(-1, Lookup(keys[2]));   // Frequency 1 is evicted.
  ASSERT_EQ(401, Lookup(keys[3]));  // The new item is retained.
}

// CHANGED: Equal-frequency entries use LRU order as the tie-breaker.
TEST_F(LFUCacheTest, EqualFrequencyLRUTieBreaker) {
  const std::vector<int> keys = KeysInSameShard(4, 200);
  Insert(keys[0], 101);  // Frequency 1, oldest.
  Insert(keys[1], 201);  // Frequency 1.
  Insert(keys[2], 301);  // Frequency 1, newest.

  // Eviction should select the oldest entry at frequency 1.
  Insert(keys[3], 401);

  ASSERT_EQ(-1, Lookup(keys[0]));  // The oldest entry is evicted.
  ASSERT_EQ(201, Lookup(keys[1]));
  ASSERT_EQ(301, Lookup(keys[2]));
  ASSERT_EQ(401, Lookup(keys[3]));
}

// CHANGED: Pinned entries remain valid while other entries are evicted.
TEST_F(LFUCacheTest, EntriesArePinned) {
  const std::vector<int> keys = KeysInSameShard(4, 300);
  Insert(keys[0], 101);
  Cache::Handle* h1 = cache_->Lookup(EncodeKey(keys[0]));

  // Access the pinned entry repeatedly.
  Lookup(keys[0]);
  Lookup(keys[0]);

  // Overfill the target shard with new entries.
  Insert(keys[1], 201);
  Insert(keys[2], 301);
  Insert(keys[3], 401);

  // Other entries may be evicted, but the pinned entry must remain valid.
  ASSERT_EQ(0, std::count(deleted_keys_.begin(), deleted_keys_.end(), keys[0]));
  ASSERT_EQ(101, DecodeValue(cache_->Value(h1)));

  cache_->Release(h1);
}

// ============================================================================
// 2Q CACHE UNIT TESTS
// ============================================================================

class TwoQCacheTest : public CacheTestBase {
 public:
  TwoQCacheTest() {
    current_ = this;
    // CHANGED: Provide four slots in one shard for deterministic queue tests.
    cache_ = New2QCache(4 * 16);
  }
};

// CHANGED: A single-access scan does not evict a frequently used Am entry.
TEST_F(TwoQCacheTest, ScanResistanceInA1in) {
  const std::vector<int> keys = KeysInSameShard(11, 10);
  // Promote one entry into Am.
  Insert(keys[0], 100);
  Lookup(keys[0]);
  Lookup(keys[0]);  // The A1In hit promotes the entry into Am.

  // Scan one-hit entries through A1In.
  for (size_t i = 1; i < keys.size(); i++) {
    Insert(keys[i], static_cast<int>(keys[i] * 10));
  }

  // The Am entry should survive the scan.
  ASSERT_EQ(100, Lookup(keys[0]));
}

// CHANGED: A1In entries move through A1Out and promote to Am on a ghost hit.
TEST_F(TwoQCacheTest, A1inToA1outAndGhostHitPromotion) {
  const std::vector<int> keys = KeysInSameShard(5, 100);
  // Insert an entry into A1In.
  Insert(keys[0], 101);

  // Push the entry into A1Out by inserting cold items.
  Insert(keys[1], 201);
  Insert(keys[2], 301);

  // The ghost retains metadata, not a value, so lookup returns a miss.
  ASSERT_EQ(-1, Lookup(keys[0]));

  // Reinsert the key while its metadata remains in A1Out.
  Insert(keys[0], 102);

  // A ghost hit promotes the entry into Am, where it survives cold inserts.
  Insert(keys[3], 401);
  Insert(keys[4], 501);

  ASSERT_EQ(102, Lookup(keys[0]));  // The promoted entry remains cached.
}

// CHANGED: Am evicts its least-recently-used entry when full.
TEST_F(TwoQCacheTest, AmEvictionOrder) {
  const std::vector<int> keys = KeysInSameShard(5, 1000);
  Insert(keys[0], 10);
  Insert(keys[1], 20);
  Insert(keys[2], 30);
  Insert(keys[0], 11);
  Insert(keys[1], 21);
  Insert(keys[3], 40);
  Insert(keys[2], 31);

  Lookup(keys[0]);  // Make keys[1] the oldest entry in Am.
  Insert(keys[4], 50);
  Insert(keys[3], 41);  // The ghost hit fills Am and evicts its LRU entry.

  ASSERT_EQ(11, Lookup(keys[0]));
  ASSERT_EQ(-1, Lookup(keys[1]));
  ASSERT_EQ(31, Lookup(keys[2]));
  ASSERT_EQ(41, Lookup(keys[3]));
}

// CHANGED: Pinned entries remain valid while 2Q queues rotate.
TEST_F(TwoQCacheTest, PinnedHandleProtection) {
  const std::vector<int> keys = KeysInSameShard(11, 200);
  Insert(keys[0], 101);
  Cache::Handle* h = cache_->Lookup(EncodeKey(keys[0]));

  // Insert many entries to force queue rotations.
  for (size_t i = 1; i < keys.size(); i++) {
    Insert(keys[i], keys[i] + 1);
  }

  // The pinned entry must remain valid in memory.
  ASSERT_EQ(101, DecodeValue(cache_->Value(h)));
  cache_->Release(h);
}

// ADDED: Regression test for a struct-layout bug. TwoQueueHandle::from was
// declared after key_data, so keys longer than one byte overwrote the queue
// tag and writing the tag corrupted the key. LevelDB's block cache uses
// 16-byte keys, which made the hash table lose entries and lookups degrade.
TEST_F(TwoQCacheTest, LongKeysKeepQueueStateIntact) {
  delete cache_;
  cache_ = New2QCache(64 * 16);  // ADDED: Sixty-four unit slots per shard.

  for (int i = 0; i < 5000; i++) {
    std::string key(16, 'k');
    EncodeFixed32(&key[4], static_cast<uint32_t>(i));  // ADDED: Bytes that overlapped the tag.
    EncodeFixed32(&key[12], static_cast<uint32_t>(i * 7));
    cache_->Release(cache_->Insert(key, EncodeValue(i + 1), 1,
                                   &CacheTestBase::BenchmarkDeleter));

    // A key must be found with its own value right after insertion, and a
    // second lookup promotes it, which writes the queue tag.
    for (int round = 0; round < 2; round++) {
      Cache::Handle* h = cache_->Lookup(key);
      ASSERT_NE(nullptr, h) << "key " << i << " lost after insert";
      ASSERT_EQ(i + 1, DecodeValue(cache_->Value(h)));
      cache_->Release(h);
    }
  }
  ASSERT_LE(cache_->TotalCharge(), static_cast<size_t>(64 * 16));
}

// ============================================================================
// ARC CACHE UNIT TESTS
// ============================================================================

class ARCCacheTest : public CacheTestBase {
 public:
  ARCCacheTest() {
    current_ = this;
    cache_ = NewARCCache(3 * 16);  // ADDED: Three slots in one shard.
  }
};

// ADDED: An entry hit twice moves from T1 to T2 and survives a scan of
// single-use keys, which only cycle through T1.
TEST_F(ARCCacheTest, RepeatedEntrySurvivesScan) {
  const std::vector<int> keys = KeysInSameShard(12, 10);
  Insert(keys[0], 100);
  ASSERT_EQ(100, Lookup(keys[0]));  // ADDED: Second access moves it to T2.

  for (size_t i = 1; i < keys.size(); i++) {
    Insert(keys[i], static_cast<int>(keys[i]));
  }

  ASSERT_EQ(100, Lookup(keys[0]));
}

// ADDED: Single-use entries in T1 are evicted oldest first.
TEST_F(ARCCacheTest, T1EvictsOldestFirst) {
  const std::vector<int> keys = KeysInSameShard(4, 100);
  Insert(keys[0], 10);
  Insert(keys[1], 20);
  Insert(keys[2], 30);
  Insert(keys[3], 40);  // ADDED: Shard is full, so keys[0] is evicted.

  ASSERT_EQ(-1, Lookup(keys[0]));
  ASSERT_EQ(20, Lookup(keys[1]));
  ASSERT_EQ(30, Lookup(keys[2]));
  ASSERT_EQ(40, Lookup(keys[3]));
}

// ADDED: A key evicted from T1 leaves a B1 ghost. Reinserting it is a ghost
// hit, so it enters T2 directly and survives later single-use inserts.
TEST_F(ARCCacheTest, B1GhostHitInsertsIntoT2) {
  const std::vector<int> keys = KeysInSameShard(9, 200);
  Insert(keys[0], 101);
  Insert(keys[1], 201);
  Insert(keys[2], 301);
  Insert(keys[3], 401);  // ADDED: Evicts keys[0] into B1.

  ASSERT_EQ(-1, Lookup(keys[0]));  // ADDED: Ghosts hold no value.

  Insert(keys[0], 102);  // ADDED: B1 hit, so the entry goes to T2.
  for (size_t i = 4; i < keys.size(); i++) {
    Insert(keys[i], static_cast<int>(keys[i]));
  }

  ASSERT_EQ(102, Lookup(keys[0]));
}

// ADDED: Erase removes the resident entry and any ghost, so a later insert
// is treated as a first access (T1) rather than a ghost hit (T2).
TEST_F(ARCCacheTest, EraseRemovesEntryAndGhost) {
  const std::vector<int> keys = KeysInSameShard(7, 300);
  Insert(keys[0], 101);
  Erase(keys[0]);
  ASSERT_EQ(-1, Lookup(keys[0]));
  ASSERT_EQ(1, std::count(deleted_keys_.begin(), deleted_keys_.end(), keys[0]));

  Insert(keys[0], 102);
  Insert(keys[1], 201);
  Insert(keys[2], 301);
  Insert(keys[3], 401);  // ADDED: Evicts keys[0] into B1.
  Erase(keys[0]);        // ADDED: Also drops the B1 ghost.

  Insert(keys[0], 103);  // ADDED: Not a ghost hit, so it lands in T1.
  Insert(keys[4], 501);
  Insert(keys[5], 601);
  Insert(keys[6], 701);  // ADDED: Three single-use inserts push it out.

  ASSERT_EQ(-1, Lookup(keys[0]));
}

// ADDED: A pinned entry stays valid while the shard is overfilled.
TEST_F(ARCCacheTest, PinnedHandleProtection) {
  const std::vector<int> keys = KeysInSameShard(11, 400);
  Insert(keys[0], 101);
  Cache::Handle* h = cache_->Lookup(EncodeKey(keys[0]));

  for (size_t i = 1; i < keys.size(); i++) {
    Insert(keys[i], static_cast<int>(keys[i]));
  }

  ASSERT_EQ(0, std::count(deleted_keys_.begin(), deleted_keys_.end(), keys[0]));
  ASSERT_EQ(101, DecodeValue(cache_->Value(h)));
  cache_->Release(h);
}

// ============================================================================
// LECAR CACHE UNIT TESTS
// ============================================================================
//
// LeCaR picks its eviction victim with LRU or LFU at random, weighted by
// learned scores, so these tests check properties that hold whichever
// expert is chosen.

class LeCaRCacheTest : public CacheTestBase {
 public:
  LeCaRCacheTest() {
    current_ = this;
    cache_ = NewLeCaRCache(3 * 16);  // ADDED: Three slots in one shard.
  }
};

// ADDED: A key that is both the most recent and the most frequent is never
// the LRU victim or the LFU victim, so it survives every eviction.
TEST_F(LeCaRCacheTest, KeyFavoredByBothExpertsSurvives) {
  const std::vector<int> keys = KeysInSameShard(40, 10);
  Insert(keys[0], 100);
  for (int i = 0; i < 5; i++) Lookup(keys[0]);

  for (size_t i = 1; i < keys.size(); i++) {
    ASSERT_EQ(100, Lookup(keys[0])) << "evicted before insert " << i;
    Insert(keys[i], static_cast<int>(keys[i]));
  }
  ASSERT_EQ(100, Lookup(keys[0]));
}

// ADDED: Reinserting a key replaces its value and deletes the old one once.
TEST_F(LeCaRCacheTest, ReinsertReplacesValue) {
  const std::vector<int> keys = KeysInSameShard(1, 100);
  Insert(keys[0], 101);
  Insert(keys[0], 102);

  ASSERT_EQ(102, Lookup(keys[0]));
  ASSERT_EQ(1u, deleted_keys_.size());
  ASSERT_EQ(101, deleted_values_[0]);
}

// ADDED: Erase removes the entry and calls the deleter exactly once.
TEST_F(LeCaRCacheTest, EraseRemovesEntry) {
  const std::vector<int> keys = KeysInSameShard(2, 200);
  Insert(keys[0], 101);
  Insert(keys[1], 201);
  Erase(keys[0]);

  ASSERT_EQ(-1, Lookup(keys[0]));
  ASSERT_EQ(201, Lookup(keys[1]));
  ASSERT_EQ(1, std::count(deleted_keys_.begin(), deleted_keys_.end(), keys[0]));
}

// ADDED: A pinned entry stays valid even if LeCaR evicts it from the cache.
TEST_F(LeCaRCacheTest, PinnedHandleProtection) {
  const std::vector<int> keys = KeysInSameShard(11, 300);
  Insert(keys[0], 101);
  Cache::Handle* h = cache_->Lookup(EncodeKey(keys[0]));

  for (size_t i = 1; i < keys.size(); i++) {
    Insert(keys[i], static_cast<int>(keys[i]));
  }

  ASSERT_EQ(0, std::count(deleted_keys_.begin(), deleted_keys_.end(), keys[0]));
  ASSERT_EQ(101, DecodeValue(cache_->Value(h)));
  cache_->Release(h);
  ASSERT_EQ(1, std::count(deleted_keys_.begin(), deleted_keys_.end(), keys[0]));
}

// ============================================================================
// PARAMETERIZED PERFORMANCE & HIT-RATE BENCHMARKS
// CHANGED: Now covers LRU, LFU, 2Q, ARC and LeCaR.
// ============================================================================

enum CacheType { kLRU, kLFU, k2Q, kARC, kLeCaR };  // CHANGED: Added ARC, LeCaR.
static const int kNumPolicies = 5;  // ADDED: Number of policies.

// ADDED: Create a cache of the given policy.
static Cache* NewCacheOfType(CacheType type, size_t capacity) {
  switch (type) {
    case kLRU:
      return NewLRUCache(capacity);
    case kLFU:
      return NewLFUCache(capacity);
    case k2Q:
      return New2QCache(capacity);
    case kARC:
      return NewARCCache(capacity);
    case kLeCaR:
      return NewLeCaRCache(capacity);
  }
  return nullptr;
}

// ADDED: Policy names for test names and output.
static const char* PolicyName(CacheType type) {
  switch (type) {
    case kLRU:
      return "LRU";
    case kLFU:
      return "LFU";
    case k2Q:
      return "2Q";
    case kARC:
      return "ARC";
    case kLeCaR:
      return "LeCaR";
  }
  return "Unknown";
}

class CacheParamTest : public CacheTestBase,
                       public testing::WithParamInterface<CacheType> {
 public:
  static constexpr int kBenchmarkCapacity = 500;

  CacheParamTest() {
    current_ = this;
    cache_ = NewCacheOfType(GetParam(), kBenchmarkCapacity);  // CHANGED: Shared helper.
  }
};

// CHANGED: Parameterized runner for LRU, LFU, 2Q, ARC and LeCaR.
INSTANTIATE_TEST_SUITE_P(
    CachePerformance, CacheParamTest,
    testing::Values(kLRU, kLFU, k2Q, kARC, kLeCaR),
    [](const testing::TestParamInfo<CacheParamTest::ParamType>& info) {
      return std::string(PolicyName(info.param));
    });

// ADDED: Every policy under LevelDB's real block-cache usage: 16-byte keys
// (table id + block offset) and byte charges of about 4 KB. Checks that a
// just-inserted block is found with its value and that usage stays within
// capacity. The policy unit tests use 4-byte keys and charge 1, which is how
// the 2Q layout bug went unnoticed.
TEST_P(CacheParamTest, LevelDBStyleKeysAndCharges) {
  const size_t kCapacity = 16 * 16 * 4096;  // ADDED: About 16 blocks per shard.
  delete cache_;
  cache_ = NewCacheOfType(GetParam(), kCapacity);

  std::mt19937 rng(99);
  std::uniform_int_distribution<int> block_dist(0, 2000);
  for (int i = 0; i < 20000; i++) {
    const int block = block_dist(rng);
    std::string key;
    PutFixed64(&key, 1);  // ADDED: Table cache id.
    PutFixed64(&key, static_cast<uint64_t>(block) * 4096);  // ADDED: Offset.

    Cache::Handle* h = cache_->Lookup(key);
    if (h != nullptr) {
      ASSERT_EQ(block, DecodeValue(cache_->Value(h)));
      cache_->Release(h);
      continue;
    }
    cache_->Release(cache_->Insert(key, EncodeValue(block), 4000 + block % 200,
                                   &CacheTestBase::BenchmarkDeleter));
    h = cache_->Lookup(key);
    ASSERT_NE(nullptr, h) << PolicyName(GetParam()) << " lost block " << block;
    ASSERT_EQ(block, DecodeValue(cache_->Value(h)));
    cache_->Release(h);
    ASSERT_LE(cache_->TotalCharge(), kCapacity);
  }
}

// measures hit rate under a uniform random read pattern
TEST_P(CacheParamTest, BenchmarkUniformHitRate) {
  const int kTotalOps = 50000;
  const int kKeySpace = 2500;

  for (int i = 0; i < kKeySpace; i++) {
    InsertForBenchmark(i, i * 2);
  }

  std::mt19937 rng(7);
  std::uniform_int_distribution<int> key_dist(0, kKeySpace - 1);

  int hits = 0;
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < kTotalOps; i++) {
    const int key = key_dist(rng);
    if (Lookup(key) != -1) {
      hits++;
    } else {
      InsertForBenchmark(key, key * 2);
    }
  }

  auto elapsed = std::chrono::high_resolution_clock::now() - start;
  const double duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  const double hit_rate = (100.0 * hits) / kTotalOps;

  std::cout << "[ PERFORMANCE ] "
            << testing::UnitTest::GetInstance()->current_test_info()->name()
            << " | Uniform Read Hit Rate: " << hit_rate << "% (" << hits << "/"
            << kTotalOps << " hits) | Latency: " << duration_ms << " ms\n";

  ASSERT_GT(hits, 0);
}

// measures hit rate under 80/20 skewed read access pattern (mimics readskewed benchmark)
TEST_P(CacheParamTest, BenchmarkSkewedHitRate) {
  const int kTotalOps = 50000;
  const int kKeySpace = 2500;  // 5x capacity to force evictions
  const int kHotRange = kKeySpace / 5;  // 20% hot keys

  // pre-fill cache
  for (int i = 0; i < kKeySpace; i++) {
    InsertForBenchmark(i, i * 2);
  }

  std::mt19937 rng(42);  // fixed seed for repeatable benchmark
  std::uniform_int_distribution<int> dist_100(0, 99);
  std::uniform_int_distribution<int> dist_hot(0, kHotRange - 1);
  std::uniform_int_distribution<int> dist_cold(kHotRange, kKeySpace - 1);

  int hits = 0;
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < kTotalOps; i++) {
    int key = (dist_100(rng) < 80) ? dist_hot(rng) : dist_cold(rng);
    int res = Lookup(key);
    if (res != -1) {
      hits++;
    } else {
      InsertForBenchmark(key, key * 2);  // reload missing key
    }
  }

  auto elapsed = std::chrono::high_resolution_clock::now() - start;
  double duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  double hit_rate = (100.0 * hits) / kTotalOps;

  std::cout << "[ PERFORMANCE ] "
            << testing::UnitTest::GetInstance()->current_test_info()->name()
            << " | 80/20 Skew Hit Rate: " << hit_rate << "% (" << hits << "/"
            << kTotalOps << " hits) | Latency: " << duration_ms << " ms\n";

  ASSERT_GT(hits, 0);
}

// measures scan resistance: ability to preserve hot working set during a large sequential scan
TEST_P(CacheParamTest, BenchmarkScanResistance) {
  const int kHotSetSize = 300;  // fits inside capacity (500)
  const int kScanSize = 2000;   // large sequential scan

  // establish a hot working set by inserting and repeatedly accessing keys
  for (int i = 0; i < kHotSetSize; i++) {
    InsertForBenchmark(i, i + 100);
    Lookup(i);
    Lookup(i);
  }

  // perform a massive sequential scan of one-hit-wonder keys
  for (int i = 10000; i < 10000 + kScanSize; i++) {
    InsertForBenchmark(i, i + 1);
  }

  // check how many hot working set entries survived the scan
  int retained_hot_keys = 0;
  for (int i = 0; i < kHotSetSize; i++) {
    if (Lookup(i) != -1) {
      retained_hot_keys++;
    }
  }

  double survival_pct = (100.0 * retained_hot_keys) / kHotSetSize;

  std::cout << "[ PERFORMANCE ] "
            << testing::UnitTest::GetInstance()->current_test_info()->name()
            << " | Scan Resistance Survival: " << survival_pct << "% ("
            << retained_hot_keys << "/" << kHotSetSize << " hot keys retained)\n";

}

// CHANGED: Compares throughput of all policies with paired, order-rotated
// workloads. Each repeat runs every policy on the same operations, in a
// rotated order, and records each policy's throughput relative to LRU.
TEST(CachePerformance, BenchmarkPairedThroughputRatios) {
  const int kOperations = 100000;
  const int kRepeats = 100;
  const int kCapacity = CacheParamTest::kBenchmarkCapacity;

  struct Operation {
    int type;
    int key;
  };

  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> op_dist(0, 9);
  std::uniform_int_distribution<int> key_dist(0, 1500);
  std::vector<Operation> operations;
  operations.reserve(kOperations);
  for (int i = 0; i < kOperations; i++) {
    operations.push_back({op_dist(rng), key_dist(rng)});
  }

  auto run_workload = [&operations](Cache* cache) {
    for (const Operation& operation : operations) {
      if (operation.type < 6) {
        Cache::Handle* handle = cache->Lookup(EncodeKey(operation.key));
        if (handle != nullptr) {
          cache->Release(handle);
        }
      } else if (operation.type < 9) {
        Cache::Handle* handle = cache->Insert(
            EncodeKey(operation.key), EncodeValue(operation.key + 1), 1,
            &CacheTestBase::BenchmarkDeleter);
        cache->Release(handle);
      } else {
        cache->Erase(EncodeKey(operation.key));
      }
    }
  };

  std::vector<double> throughput_samples[kNumPolicies];
  std::vector<double> ratio_to_lru[kNumPolicies];
  for (int p = 0; p < kNumPolicies; p++) {
    throughput_samples[p].reserve(kRepeats);
    ratio_to_lru[p].reserve(kRepeats);
  }

  for (int repeat = 0; repeat < kRepeats; repeat++) {
    Cache* caches[kNumPolicies];
    int order[kNumPolicies];
    for (int p = 0; p < kNumPolicies; p++) {
      caches[p] = NewCacheOfType(static_cast<CacheType>(p), kCapacity);
      order[p] = (repeat + p) % kNumPolicies;
    }

    // Warm each policy with identical work before the timed pass.
    for (int policy : order) {
      run_workload(caches[policy]);
    }

    double operations_per_second[kNumPolicies];
    for (int policy : order) {
      const auto start = std::chrono::steady_clock::now();
      run_workload(caches[policy]);
      const std::chrono::duration<double> elapsed =
          std::chrono::steady_clock::now() - start;
      operations_per_second[policy] = kOperations / elapsed.count();
      throughput_samples[policy].push_back(operations_per_second[policy]);
    }
    for (int p = 0; p < kNumPolicies; p++) {
      ratio_to_lru[p].push_back(operations_per_second[p] /
                                operations_per_second[kLRU]);
    }

    for (Cache* cache : caches) {
      delete cache;
    }
  }

  // Returns {median, min, max}.
  auto summarize = [](std::vector<double>* values) {
    std::sort(values->begin(), values->end());
    return std::vector<double>{(*values)[values->size() / 2], values->front(),
                               values->back()};
  };

  for (int p = 0; p < kNumPolicies; p++) {
    const std::vector<double> throughput = summarize(&throughput_samples[p]);
    std::cout << "[ PERFORMANCE ] BenchmarkPairedThroughputRatios | "
              << PolicyName(static_cast<CacheType>(p)) << " median "
              << static_cast<uint64_t>(throughput[0]) << " ops/sec";
    if (p != kLRU) {
      const std::vector<double> ratio = summarize(&ratio_to_lru[p]);
      const bool inconclusive = ratio[1] <= 1.0 && ratio[2] >= 1.0;
      std::cout << " | ratio to LRU: median " << ratio[0] << " range ["
                << ratio[1] << ", " << ratio[2] << "]"
                << (inconclusive ? " cannot distinguish throughput" : "");
      ASSERT_GT(ratio[0], 0);
    }
    std::cout << " (" << kRepeats << " paired repetitions)\n";
  }
}

}  // namespace leveldb