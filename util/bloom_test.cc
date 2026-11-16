// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "leveldb/filter_policy.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/testutil.h"

namespace leveldb {

static const int kVerbose = 1;

static Slice Key(int i, char* buffer) {
  EncodeFixed32(buffer, i);
  return Slice(buffer, sizeof(uint32_t));
}

class BloomTest : public testing::Test {
 public:
  BloomTest() : policy_(NewBloomFilterPolicy(10)) {}

  ~BloomTest() { delete policy_; }

  void Reset() {
    keys_.clear();
    filter_.clear();
  }

  void Add(const Slice& s) { keys_.push_back(s.ToString()); }

  void Build() {
    std::vector<Slice> key_slices;
    for (size_t i = 0; i < keys_.size(); i++) {
      key_slices.push_back(Slice(keys_[i]));
    }
    filter_.clear();
    policy_->CreateFilter(&key_slices[0], static_cast<int>(key_slices.size()),
                          &filter_);
    keys_.clear();
    if (kVerbose >= 2) DumpFilter();
  }

  size_t FilterSize() const { return filter_.size(); }

  void DumpFilter() {
    std::fprintf(stderr, "F(");
    for (size_t i = 0; i + 1 < filter_.size(); i++) {
      const unsigned int c = static_cast<unsigned int>(filter_[i]);
      for (int j = 0; j < 8; j++) {
        std::fprintf(stderr, "%c", (c & (1 << j)) ? '1' : '.');
      }
    }
    std::fprintf(stderr, ")\n");
  }

  bool Matches(const Slice& s) {
    if (!keys_.empty()) {
      Build();
    }
    return policy_->KeyMayMatch(s, filter_);
  }

  double FalsePositiveRate() {
    char buffer[sizeof(int)];
    int result = 0;
    for (int i = 0; i < 10000; i++) {
      if (Matches(Key(i + 1000000000, buffer))) {
        result++;
      }
    }
    return result / 10000.0;
  }

 private:
  const FilterPolicy* policy_;
  std::string filter_;
  std::vector<std::string> keys_;
};

TEST_F(BloomTest, EmptyFilter) {
  ASSERT_TRUE(!Matches("hello"));
  ASSERT_TRUE(!Matches("world"));
}

TEST_F(BloomTest, Small) {
  Add("hello");
  Add("world");
  ASSERT_TRUE(Matches("hello"));
  ASSERT_TRUE(Matches("world"));
  ASSERT_TRUE(!Matches("x"));
  ASSERT_TRUE(!Matches("foo"));
}

static int NextLength(int length) {
  if (length < 10) {
    length += 1;
  } else if (length < 100) {
    length += 10;
  } else if (length < 1000) {
    length += 100;
  } else {
    length += 1000;
  }
  return length;
}

TEST_F(BloomTest, VaryingLengths) {
  char buffer[sizeof(int)];

  // Count number of filters that significantly exceed the false positive rate
  int mediocre_filters = 0;
  int good_filters = 0;

  for (int length = 1; length <= 10000; length = NextLength(length)) {
    Reset();
    for (int i = 0; i < length; i++) {
      Add(Key(i, buffer));
    }
    Build();

    ASSERT_LE(FilterSize(), static_cast<size_t>((length * 10 / 8) + 40))
        << length;

    // All added keys must match
    for (int i = 0; i < length; i++) {
      ASSERT_TRUE(Matches(Key(i, buffer)))
          << "Length " << length << "; key " << i;
    }

    // Check false positive rate
    double rate = FalsePositiveRate();
    if (kVerbose >= 1) {
      std::fprintf(stderr,
                   "False positives: %5.2f%% @ length = %6d ; bytes = %6d\n",
                   rate * 100.0, length, static_cast<int>(FilterSize()));
    }
    ASSERT_LE(rate, 0.02);  // Must not be over 2%
    if (rate > 0.0125)
      mediocre_filters++;  // Allowed, but not too often
    else
      good_filters++;
  }
  if (kVerbose >= 1) {
    std::fprintf(stderr, "Filters: %d good, %d mediocre\n", good_filters,
                 mediocre_filters);
  }
  ASSERT_LE(mediocre_filters, good_filters / 5);
}

// Different bits-per-byte

// ============================================================================
// ADDED: Bits-per-key sweep (filter-only microbenchmark).
//
// For each bits-per-key setting, builds many filters, probes them with keys
// that were never added, and compares the measured false-positive rate with
// the textbook estimate (1 - e^{-kn/m})^k for the filter LevelDB actually
// builds (k = floor(0.69 * bits), m >= 64 bits). Two filter sizes are run:
//   keys_per_filter = 40     about one 4 KB data block of 100-byte values,
//                            which is what a real SSTable filter covers
//   keys_per_filter = 100000 the asymptotic case most analyses assume
// Each is run with sequential (db_bench-style) and random keys.
// Output lines start with BLOOM_SWEEP so they can be grepped into a CSV.
// ============================================================================

// Sequential keys use db_bench's "%016d" format. Random keys are the same
// width but scrambled, so they show how the filter behaves when key bytes
// are well mixed (LevelDB's hash has no final avalanche step, so nearly
// identical keys can land on correlated bit positions in small filters).
static std::string SweepKey(int i, bool random) {
  uint64_t v = static_cast<uint64_t>(i);
  if (random) {  // splitmix64
    v += 0x9e3779b97f4a7c15ULL;
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    v ^= v >> 31;
    v %= 10000000000000000ULL;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llu",
                static_cast<unsigned long long>(v));
  return std::string(buf, 16);
}

static int LevelDBProbeCount(int bits_per_key) {
  int k = static_cast<int>(bits_per_key * 0.69);  // Mirrors util/bloom.cc.
  if (k < 1) k = 1;
  if (k > 30) k = 30;
  return k;
}

static double TheoreticalFpr(int bits_per_key, int n) {
  size_t bits = static_cast<size_t>(n) * bits_per_key;
  if (bits < 64) bits = 64;  // Mirrors util/bloom.cc.
  bits = ((bits + 7) / 8) * 8;
  const double k = LevelDBProbeCount(bits_per_key);
  return std::pow(1.0 - std::exp(-k * n / static_cast<double>(bits)), k);
}

TEST(BloomSweep, BitsPerKeyVsFalsePositiveRate) {
  const int kBitsPerKey[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 20};
  const int kKeysPerFilter[] = {40, 100000};
  const int kTotalKeys = 200000;   // Keys inserted per (bits, size) setting.
  const int kTotalProbes = 2000000;  // Absent-key probes per setting.

  std::fprintf(stdout,
               "BLOOM_SWEEP,key_style,keys_per_filter,bits_per_key,k,filters,"
               "filter_bytes_per_key,measured_fpr,theoretical_fpr,"
               "ns_per_probe\n");

  for (bool random_keys : {false, true}) {
  for (int keys_per_filter : kKeysPerFilter) {
    double previous_fpr = 1.0;
    for (int bits : kBitsPerKey) {
      std::unique_ptr<const FilterPolicy> policy(NewBloomFilterPolicy(bits));
      const int num_filters = kTotalKeys / keys_per_filter;

      // Build filters over disjoint key ranges [f*n, (f+1)*n).
      std::vector<std::string> filters(num_filters);
      size_t filter_bytes = 0;
      for (int f = 0; f < num_filters; f++) {
        std::vector<std::string> keys;
        keys.reserve(keys_per_filter);
        for (int i = 0; i < keys_per_filter; i++) {
          keys.push_back(SweepKey(f * keys_per_filter + i, random_keys));
        }
        std::vector<Slice> slices(keys.begin(), keys.end());
        policy->CreateFilter(slices.data(), keys_per_filter, &filters[f]);
        filter_bytes += filters[f].size();

        // No false negatives, ever.
        for (const Slice& key : slices) {
          ASSERT_TRUE(policy->KeyMayMatch(key, filters[f]));
        }
      }

      // Probe with keys that were never inserted anywhere.
      const int probes_per_filter = kTotalProbes / num_filters;
      std::vector<std::string> probe_keys;
      probe_keys.reserve(probes_per_filter);
      for (int i = 0; i < probes_per_filter; i++) {
        probe_keys.push_back(SweepKey(1000000000 + i, random_keys));
      }
      uint64_t false_positives = 0;
      const auto start = std::chrono::steady_clock::now();
      for (const std::string& filter : filters) {
        for (const std::string& key : probe_keys) {
          false_positives += policy->KeyMayMatch(key, filter) ? 1 : 0;
        }
      }
      const std::chrono::duration<double, std::nano> elapsed =
          std::chrono::steady_clock::now() - start;
      const double probes =
          static_cast<double>(probes_per_filter) * num_filters;
      const double measured = false_positives / probes;
      const double theory = TheoreticalFpr(bits, keys_per_filter);

      std::fprintf(stdout, "BLOOM_SWEEP,%s,%d,%d,%d,%d,%.3f,%.6f,%.6f,%.2f\n",
                   random_keys ? "random" : "sequential", keys_per_filter, bits, LevelDBProbeCount(bits),
                   num_filters,
                   static_cast<double>(filter_bytes) / kTotalKeys, measured,
                   theory, elapsed.count() / probes);

      // Sanity checks on well-mixed keys: the measured rate stays near the
      // estimate, and more bits per key never makes the filter meaningfully
      // worse. The second check is skipped below the 64-bit minimum filter
      // size, where small settings are rounded up and can beat larger ones.
      // Sequential keys are reported but not checked: their rate depends on
      // hash correlations, which is exactly what the comparison shows.
      if (random_keys) {
        ASSERT_LE(measured, 2.0 * theory + 0.002)
            << "bits=" << bits << " keys_per_filter=" << keys_per_filter;
        if (keys_per_filter * bits >= 64) {
          ASSERT_LE(measured, previous_fpr + 0.002) << "bits=" << bits;
        }
      }
      previous_fpr = measured;
    }
  }
  }
}

}  // namespace leveldb
