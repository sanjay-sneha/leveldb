// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// ADDED: Process-wide counters for evaluating Bloom filter effectiveness.
//
// All counters are updated from Table::InternalGet (the point-lookup path)
// and Table::ReadFilter / ~Table (filter memory). Compactions and iterators
// are not counted, so the numbers describe Get() traffic only.

#ifndef STORAGE_LEVELDB_TABLE_BLOOM_STATS_H_
#define STORAGE_LEVELDB_TABLE_BLOOM_STATS_H_

#include <cstdint>
#include <string>

namespace leveldb {

struct BloomStats {
  // Per-table point lookups that reached a table with a filter.
  uint64_t filter_checks = 0;
  // Filter said "definitely not here": a data block read was avoided.
  uint64_t filter_negatives = 0;
  // Filter said "maybe" and the key really was in the block.
  uint64_t true_positives = 0;
  // Filter said "maybe" but the key was not in the block (wasted block read).
  uint64_t false_positives = 0;
  // Per-table point lookups on tables that have no filter.
  uint64_t unfiltered_lookups = 0;

  // Data blocks needed by Get(): served from the block cache vs. read from
  // the file (which may still be served by the OS page cache).
  uint64_t block_cache_hits = 0;
  uint64_t block_reads = 0;

  // Filter blocks loaded when tables are opened (grows when the table cache
  // evicts and reopens tables) and filter bytes currently held in memory.
  uint64_t filter_loads = 0;
  uint64_t filter_bytes_loaded = 0;
  uint64_t filter_bytes_resident = 0;

  // False positives / (false positives + true negatives). Only lookups for
  // keys that were absent from the probed table are in the denominator.
  double FalsePositiveRate() const {
    const uint64_t absent = false_positives + filter_negatives;
    return absent == 0 ? 0.0 : static_cast<double>(false_positives) / absent;
  }

  std::string ToString() const;
};

// Snapshot of the global counters.
BloomStats GetBloomStats();

// Zero every counter except filter_bytes_resident, which tracks live memory.
void ResetBloomStats();

namespace bloom_stats_internal {
void RecordFilterNegative();
void RecordFilterPositive(bool key_present);
void RecordUnfilteredLookup();
void RecordBlockCacheHit();
void RecordBlockRead();
void RecordFilterLoaded(uint64_t bytes);
void RecordFilterReleased(uint64_t bytes);
}  // namespace bloom_stats_internal

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOOM_STATS_H_
