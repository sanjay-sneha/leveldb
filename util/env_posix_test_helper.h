// Copyright 2017 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_ENV_POSIX_TEST_HELPER_H_
#define STORAGE_LEVELDB_UTIL_ENV_POSIX_TEST_HELPER_H_

namespace leveldb {

class EnvPosixTest;
void SetReadOnlyMMapLimitForBenchmarks(int limit);  // ADDED: see below.

// A helper for the POSIX Env to facilitate testing.
class EnvPosixTestHelper {
 private:
  friend class EnvPosixTest;
  friend void SetReadOnlyMMapLimitForBenchmarks(int limit);  // ADDED

  // Set the maximum number of read-only files that will be opened.
  // Must be called before creating an Env.
  static void SetReadOnlyFDLimit(int limit);

  // Set the maximum number of read-only files that will be mapped via mmap.
  // Must be called before creating an Env.
  static void SetReadOnlyMMapLimit(int limit);
};

// ADDED: Benchmark hook with the same effect as SetReadOnlyMMapLimit().
// Passing 0 makes SSTables use pread() instead of mmap, so data blocks are
// heap-allocated and can be inserted into the block cache even when
// compression is disabled. Must be called before Env::Default().
void SetReadOnlyMMapLimitForBenchmarks(int limit);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_ENV_POSIX_TEST_HELPER_H_
