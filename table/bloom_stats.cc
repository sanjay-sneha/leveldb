// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// ADDED: Bloom filter effectiveness counters. See bloom_stats.h.

#include "table/bloom_stats.h"

#include <atomic>
#include <cstdio>

namespace leveldb {

namespace {

// Relaxed atomics: counters are independent and only read after a benchmark.
std::atomic<uint64_t> g_filter_checks{0};
std::atomic<uint64_t> g_filter_negatives{0};
std::atomic<uint64_t> g_true_positives{0};
std::atomic<uint64_t> g_false_positives{0};
std::atomic<uint64_t> g_unfiltered_lookups{0};
std::atomic<uint64_t> g_block_cache_hits{0};
std::atomic<uint64_t> g_block_reads{0};
std::atomic<uint64_t> g_filter_loads{0};
std::atomic<uint64_t> g_filter_bytes_loaded{0};
std::atomic<uint64_t> g_filter_bytes_resident{0};

inline void Bump(std::atomic<uint64_t>* counter, uint64_t n = 1) {
  counter->fetch_add(n, std::memory_order_relaxed);
}

inline uint64_t Load(const std::atomic<uint64_t>& counter) {
  return counter.load(std::memory_order_relaxed);
}

}  // namespace

std::string BloomStats::ToString() const {
  char buf[512];
  std::snprintf(
      buf, sizeof(buf),
      "filter_checks=%llu filter_negatives=%llu true_positives=%llu "
      "false_positives=%llu fpr=%.6f unfiltered_lookups=%llu "
      "block_cache_hits=%llu block_reads=%llu filter_loads=%llu "
      "filter_bytes_loaded=%llu filter_bytes_resident=%llu",
      static_cast<unsigned long long>(filter_checks),
      static_cast<unsigned long long>(filter_negatives),
      static_cast<unsigned long long>(true_positives),
      static_cast<unsigned long long>(false_positives), FalsePositiveRate(),
      static_cast<unsigned long long>(unfiltered_lookups),
      static_cast<unsigned long long>(block_cache_hits),
      static_cast<unsigned long long>(block_reads),
      static_cast<unsigned long long>(filter_loads),
      static_cast<unsigned long long>(filter_bytes_loaded),
      static_cast<unsigned long long>(filter_bytes_resident));
  return buf;
}

BloomStats GetBloomStats() {
  BloomStats s;
  s.filter_checks = Load(g_filter_checks);
  s.filter_negatives = Load(g_filter_negatives);
  s.true_positives = Load(g_true_positives);
  s.false_positives = Load(g_false_positives);
  s.unfiltered_lookups = Load(g_unfiltered_lookups);
  s.block_cache_hits = Load(g_block_cache_hits);
  s.block_reads = Load(g_block_reads);
  s.filter_loads = Load(g_filter_loads);
  s.filter_bytes_loaded = Load(g_filter_bytes_loaded);
  s.filter_bytes_resident = Load(g_filter_bytes_resident);
  return s;
}

void ResetBloomStats() {
  for (std::atomic<uint64_t>* c :
       {&g_filter_checks, &g_filter_negatives, &g_true_positives,
        &g_false_positives, &g_unfiltered_lookups, &g_block_cache_hits,
        &g_block_reads, &g_filter_loads, &g_filter_bytes_loaded}) {
    c->store(0, std::memory_order_relaxed);
  }
}

namespace bloom_stats_internal {

void RecordFilterNegative() {
  Bump(&g_filter_checks);
  Bump(&g_filter_negatives);
}

void RecordFilterPositive(bool key_present) {
  Bump(&g_filter_checks);
  Bump(key_present ? &g_true_positives : &g_false_positives);
}

void RecordUnfilteredLookup() { Bump(&g_unfiltered_lookups); }
void RecordBlockCacheHit() { Bump(&g_block_cache_hits); }
void RecordBlockRead() { Bump(&g_block_reads); }

void RecordFilterLoaded(uint64_t bytes) {
  Bump(&g_filter_loads);
  Bump(&g_filter_bytes_loaded, bytes);
  Bump(&g_filter_bytes_resident, bytes);
}

void RecordFilterReleased(uint64_t bytes) {
  g_filter_bytes_resident.fetch_sub(bytes, std::memory_order_relaxed);
}

}  // namespace bloom_stats_internal

}  // namespace leveldb
