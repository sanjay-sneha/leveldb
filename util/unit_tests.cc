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

// compares cache throughput with paired, order-rotated workloads
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

  std::vector<double> throughput_samples[3];
  std::vector<double> lfu_lru_ratios;
  std::vector<double> twoq_lru_ratios;
  for (std::vector<double>& samples : throughput_samples) {
    samples.reserve(kRepeats);
  }
  lfu_lru_ratios.reserve(kRepeats);
  twoq_lru_ratios.reserve(kRepeats);
  for (int repeat = 0; repeat < kRepeats; repeat++) {
    Cache* caches[] = {NewLRUCache(kCapacity), NewLFUCache(kCapacity),
                       New2QCache(kCapacity)};
    const int order[] = {repeat % 3, (repeat + 1) % 3, (repeat + 2) % 3};

    // Warm each policy with identical work before the timed pass.
    for (int policy : order) {
        run_workload(caches[policy]);
    }

    double operations_per_second[3];
    for (int policy : order) {
      const auto start = std::chrono::steady_clock::now();
      run_workload(caches[policy]);
      const std::chrono::duration<double> elapsed =
          std::chrono::steady_clock::now() - start;
      operations_per_second[policy] = kOperations / elapsed.count();
        throughput_samples[policy].push_back(operations_per_second[policy]);
    }

    lfu_lru_ratios.push_back(operations_per_second[kLFU] /
                             operations_per_second[kLRU]);
    twoq_lru_ratios.push_back(operations_per_second[k2Q] /
                              operations_per_second[kLRU]);

    for (Cache* cache : caches) {
      delete cache;
    }
  }

  auto summarize = [](std::vector<double>* ratios) {
    std::sort(ratios->begin(), ratios->end());
    return std::vector<double>{(*ratios)[ratios->size() / 2], ratios->front(),
                               ratios->back()};
  };
  const std::vector<double> lru_throughput = summarize(&throughput_samples[kLRU]);
  const std::vector<double> lfu_throughput = summarize(&throughput_samples[kLFU]);
  const std::vector<double> twoq_throughput = summarize(&throughput_samples[k2Q]);
  const std::vector<double> lfu_summary = summarize(&lfu_lru_ratios);
  const std::vector<double> twoq_summary = summarize(&twoq_lru_ratios);
  const bool lfu_inconclusive =
      lfu_summary[1] <= 1.0 && lfu_summary[2] >= 1.0;
  const bool twoq_inconclusive =
      twoq_summary[1] <= 1.0 && twoq_summary[2] >= 1.0;

  std::cout << "[ PERFORMANCE ] "
            << testing::UnitTest::GetInstance()->current_test_info()->name()
            << " | Median throughput: LRU "
            << static_cast<uint64_t>(lru_throughput[0]) << " ops/sec, LFU "
            << static_cast<uint64_t>(lfu_throughput[0]) << " ops/sec, 2Q "
            << static_cast<uint64_t>(twoq_throughput[0]) << " ops/sec"
            << " | LFU/LRU median ratio: " << lfu_summary[0] << " range ["
            << lfu_summary[1] << ", " << lfu_summary[2] << "]"
            << (lfu_inconclusive ? " cannot distinguish throughput" : "")
            << " | 2Q/LRU median ratio: " << twoq_summary[0] << " range ["
            << twoq_summary[1] << ", " << twoq_summary[2] << "]"
            << (twoq_inconclusive ? " cannot distinguish throughput" : "")
            << " (" << kRepeats << " paired repetitions)\n";

  ASSERT_GT(lfu_summary[0], 0);
  ASSERT_GT(twoq_summary[0], 0);
}

}  // namespace leveldb