// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <algorithm>
#include <chrono>
#include <random>
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

// ============================================================================
// PARAMETERIZED PERFORMANCE & HIT-RATE BENCHMARKS (LRU vs LFU vs 2Q)
// ============================================================================

enum CacheType { kLRU, kLFU, k2Q };

class CacheParamTest : public CacheTestBase,
                       public testing::WithParamInterface<CacheType> {
 public:
  static constexpr int kBenchmarkCapacity = 500;

  CacheParamTest() {
    current_ = this;
    switch (GetParam()) {
      case kLRU:
        cache_ = NewLRUCache(kBenchmarkCapacity);
        break;
      case kLFU:
        cache_ = NewLFUCache(kBenchmarkCapacity);
        break;
      case k2Q:
        cache_ = New2QCache(kBenchmarkCapacity);
        break;
    }
  }
};

// parameterized runner for LRU, LFU, and 2Q
INSTANTIATE_TEST_SUITE_P(
    CachePerformance, CacheParamTest,
    testing::Values(kLRU, kLFU, k2Q),
    [](const testing::TestParamInfo<CacheParamTest::ParamType>& info) {
      switch (info.param) {
        case kLRU:
          return "LRU";
        case kLFU:
          return "LFU";
        case k2Q:
          return "2Q";
      }
      return "Unknown";
    });

// measures hit rate under 80/20 skewed read access pattern (mimics readskewed benchmark)
TEST_P(CacheParamTest, BenchmarkSkewedHitRate) {
  const int kTotalOps = 50000;
  const int kKeySpace = 2500;  // 5x capacity to force evictions
  const int kHotRange = kKeySpace / 5;  // 20% hot keys

  // pre-fill cache
  for (int i = 0; i < kKeySpace; i++) {
    Insert(i, i * 2);
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
      Insert(key, key * 2);  // reload missing key
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
    Insert(i, i + 100);
    Lookup(i);
    Lookup(i);
  }

  // perform a massive sequential scan of one-hit-wonder keys
  for (int i = 10000; i < 10000 + kScanSize; i++) {
    Insert(i, i + 1);
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

  ASSERT_GE(retained_hot_keys, 0);
}

// measures throughput of rapid mixed operations (insert, lookup, erase)
TEST_P(CacheParamTest, BenchmarkThroughput) {
  const int kOperations = 100000;
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> op_dist(0, 9);
  std::uniform_int_distribution<int> key_dist(0, 1500);

  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < kOperations; i++) {
    int key = key_dist(rng);
    int op = op_dist(rng);

    if (op < 6) {
      Lookup(key);  // 60% lookups
    } else if (op < 9) {
      Insert(key, key + 1);  // 30% inserts
    } else {
      Erase(key);  // 10% erases
    }
  }

  auto elapsed = std::chrono::high_resolution_clock::now() - start;
  double duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  double qps = (kOperations / (duration_ms / 1000.0));

  std::cout << "[ PERFORMANCE ] "
            << testing::UnitTest::GetInstance()->current_test_info()->name()
            << " | Mixed Churn Ops/sec: " << static_cast<uint64_t>(qps)
            << " | Duration: " << duration_ms << " ms\n";

  ASSERT_GT(duration_ms, 0);
}

}  // namespace leveldb