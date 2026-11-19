# CS598APE Project: Cache Eviction Policies and Bloom Filters in LevelDB
 
**Team:** Sneha Dhital, Naomi Lin
 
This fork of [LevelDB](https://github.com/google/leveldb) evaluates how the
block cache eviction policy and the Bloom filter size affect read performance.
We added four eviction policies alongside LevelDB's default sharded LRU cache
(LFU, 2Q, ARC and LeCaR), instrumented LevelDB's Bloom filters, and benchmarked
everything on synthetic workloads and a real Twitter production cache trace.
 
The original LevelDB README is below this project section.
 
## Hardware and software requirements
 
We ran the experiments in the report on the course VM:
 
| | |
|---|---|
| CPU | 4 × Intel Xeon Silver 4216 @ 2.10 GHz (22.5 MB cache) |
| Memory | 15 GB RAM |
| Disk | ~218 GB free |
| OS | Ubuntu Linux |
 
To build and run:
 
- Linux/macOS with a C++17 compiler (g++ or clang) and CMake 3.22 or newer.
- Git submodules (GoogleTest and Google Benchmark) checked out.
- About 5 GB of free disk for the Twitter trace experiments (trace slice plus database copies).
- `sudo` on Linux for the trace experiments, which use a cgroup memory cap
  (`systemd-run`) and drop the OS page cache between runs.
- `valgrind` for memory checking, `zstd` and `curl` to download the trace.

Snappy is not required. However, without Snappy, LevelDB memory-maps
table files and never inserts their blocks into the block cache, so all cache
experiments have to be run with `--use_mmap=0` (and the provided scripts do this).
 
## Building
 
```
git clone --recurse-submodules https://github.com/sanjay-sneha/leveldb
cd leveldb
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j leveldb_tests db_bench
```
 
## What we changed
 
| File | Status | Purpose |
|---|---|---|
| `util/cache_lfu.cc` | added | LFU cache: per-frequency buckets (kept in an `std::map`), LRU order within each frequency for tie-breaking |
| `util/cache_2q.cc` | added | 2Q cache (A1in FIFO, A1out ghost queue, Am LRU) |
| `util/cache_arc.cc` | added | ARC cache (T1/T2 resident lists, B1/B2 ghost lists, adaptive target) |
| `util/cache_lecar.cc` | added | LeCaR cache (learned weighting between LRU and LFU experts) |
| `include/leveldb/cache.h` | changed | Factory functions `NewLFUCache`, `New2QCache`, `NewARCCache`, `NewLeCaRCache` |
| `util/unit_tests.cc` | added | Unit tests and small benchmarks for all cache policies |
| `table/bloom_stats.h`, `table/bloom_stats.cc` | added | Counters for Bloom filter checks, true/false positives, block reads, filter memory |
| `table/table.cc` | changed | Records the Bloom counters on the point-lookup path |
| `util/bloom_test.cc` | changed | `BloomSweep` test: measured vs. theoretical false-positive rate per bits-per-key |
| `util/env_posix.cc`, `util/env_posix_test_helper.h` | changed | Hook used by `db_bench --use_mmap=0` |
| `util/histogram.h` | changed | Exposes percentiles so db_bench can report p50/p99 |
| `benchmarks/db_bench.cc` | changed | New flags and benchmarks (see below) |
| `benchmarks/bloom_sweep.sh` | added | Synthetic bits-per-key sweep across workloads |
| `benchmarks/trace_sweep.sh` | added | Twitter trace replay across all cache policies |
| `CMakeLists.txt` | changed | Builds the new source files |
 
The modified regions are marked with `ADDED:` or `CHANGED:` comments.
 
## Running the tests
 
All project unit tests:
 
```
./leveldb_tests --gtest_filter='LFU*:TwoQ*:ARC*:LeCaR*:CachePerformance*:Bloom*'
```
 
- `LFUCacheTest.*`, `TwoQCacheTest.*`, `ARCCacheTest.*`, `LeCaRCacheTest.*`:
  correctness of each policy (eviction order, ghost entries, scan resistance,
  erase, pinned handles). The tests use `KeysInSameShard` to place keys in one of
  the 16 shards so that eviction is deterministic.
- `CachePerformance/CacheParamTest.*`: run for all five policies:
  LevelDB-style keys and charges, uniform and 80/20 hit rate, scan resistance.
- `CachePerformance.BenchmarkPairedThroughputRatios`: throughput of all
  policies on identical operations, 100 paired, order-rotated repetitions.
- `BloomSweep.BitsPerKeyVsFalsePositiveRate`: prints `BLOOM_SWEEP` CSV rows.

Run the whole LevelDB suite with `./leveldb_tests`.
 
### Valgrind
 
```
valgrind --leak-check=full ./leveldb_tests --gtest_filter='LFU*:TwoQ*:ARC*:LeCaR*:CachePerformance/*:BloomTest*'
```
 
Expected: `ERROR SUMMARY: 0 errors` and no leaks. End-to-end db_bench runs
report 0 bytes definitely lost; the only report is 320 bytes "possibly lost"
from upstream LevelDB's background compaction thread, which LevelDB
intentionally never joins.
 
## Benchmarks (db_bench)
 
New `db_bench` flags:
 
| Flag | Meaning |
|---|---|
| `--cache_type=lru\|lfu\|2q\|arc\|lecar` | Block cache eviction policy |
| `--use_mmap=0` | Read tables with `pread` so blocks enter the block cache |
| `--read_dist=uniform\|zipfian`, `--zipf_theta=0.99` | Key distribution for `readmix` |
| `--missing_frac=F` | Fraction of `readmix` lookups for absent keys |
| `--bloom_stats=1` | Print a `RESULT` CSV line per benchmark |
| `--trace=FILE`, `--trace_ops=N` | Twitter trace for `tracefill` / `tracereplay` |
 
New benchmarks: `fillperm` (each key written exactly once, shuffled),
`readmix`, `readskewed` (Zipfian), `tracefill`, `tracereplay`.
 
Example:
 
```
./db_bench --benchmarks=fillperm,compact,readmix --num=1000000 --use_mmap=0 \
  --bloom_bits=10 --cache_type=arc --read_dist=zipfian --missing_frac=0.5 \
  --histogram=1 --bloom_stats=1
```
 
### Bloom filter sweep
 
```
./leveldb_tests --gtest_filter='BloomSweep*' | grep BLOOM_SWEEP > bloom_sweep.csv
../benchmarks/bloom_sweep.sh          # end-to-end; rebuilds the DB per setting
```
 
### Twitter trace replay
 
We used cluster 52 of the Twitter cache traces
([twitter/cache-trace](https://github.com/twitter/cache-trace), Yang et al., OSDI 2020),
which the trace authors recommend for eviction studies. Download the first 20M requests from here:
 
```
mkdir -p ~/data && cd ~/data
curl -s https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/open_source/cluster52.sort.zst \
  | zstd -dc | head -n 20000000 > twitter52.csv
```
 
Run every policy, with and without a 10-bit Bloom filter:
 
```
cd ~/leveldb/build
TRACE=~/data/twitter52.csv ../benchmarks/trace_sweep.sh
```
 
The script loads the trace's keys once per Bloom setting, replays each policy
on a fresh copy of that database under a 256 MB memory cap, and writes
`trace_sweep.csv`. The settings can be changed with environment variables
(`TRACE_OPS`, `CACHE_TYPES`, `BLOOM_BITS`, `MEMCAP`, `REPEATS`).
 
## Known limitations
 
- Concurrency of the new caches is not specifically tested.
- LeCaR's learned weights are not checked directly by unit tests.
- The 2Q ghost-list size limit is not covered by a dedicated test.

---
 
# Original LevelDB README

LevelDB is a fast key-value storage library written at Google that provides an ordered mapping from string keys to string values.

> **This repository is receiving very limited maintenance. We will only review the following types of changes.**
>
> * Fixes for critical bugs, such as data loss or memory corruption
> * Changes absolutely needed by internally supported leveldb clients. These typically fix breakage introduced by a language/standard library/OS update

[![ci](https://github.com/google/leveldb/actions/workflows/build.yml/badge.svg)](https://github.com/google/leveldb/actions/workflows/build.yml)

Authors: Sanjay Ghemawat (sanjay@google.com) and Jeff Dean (jeff@google.com)

# Features

  * Keys and values are arbitrary byte arrays.
  * Data is stored sorted by key.
  * Callers can provide a custom comparison function to override the sort order.
  * The basic operations are `Put(key,value)`, `Get(key)`, `Delete(key)`.
  * Multiple changes can be made in one atomic batch.
  * Users can create a transient snapshot to get a consistent view of data.
  * Forward and backward iteration is supported over the data.
  * Data is automatically compressed using the [Snappy compression library](https://google.github.io/snappy/), but [Zstd compression](https://facebook.github.io/zstd/) is also supported.
  * External activity (file system operations etc.) is relayed through a virtual interface so users can customize the operating system interactions.

# Documentation

  [LevelDB library documentation](https://github.com/google/leveldb/blob/main/doc/index.md) is online and bundled with the source code.

# Limitations

  * This is not a SQL database.  It does not have a relational data model, it does not support SQL queries, and it has no support for indexes.
  * Only a single process (possibly multi-threaded) can access a particular database at a time.
  * There is no client-server support builtin to the library.  An application that needs such support will have to wrap their own server around the library.

# Getting the Source

```bash
git clone --recurse-submodules https://github.com/google/leveldb.git
```

# Building

This project supports [CMake](https://cmake.org/) out of the box.

### Build for POSIX

Quick start:

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
```

### Building for Windows

First generate the Visual Studio 2017 project/solution files:

```cmd
mkdir build
cd build
cmake -G "Visual Studio 15" ..
```
The default default will build for x86. For 64-bit run:

```cmd
cmake -G "Visual Studio 15 Win64" ..
```

To compile the Windows solution from the command-line:

```cmd
devenv /build Debug leveldb.sln
```

or open leveldb.sln in Visual Studio and build from within.

Please see the CMake documentation and `CMakeLists.txt` for more advanced usage.

# Contributing to the leveldb Project

> **This repository is receiving very limited maintenance. We will only review the following types of changes.**
>
> * Bug fixes
> * Changes absolutely needed by internally supported leveldb clients. These typically fix breakage introduced by a language/standard library/OS update

The leveldb project welcomes contributions. leveldb's primary goal is to be
a reliable and fast key/value store. Changes that are in line with the
features/limitations outlined above, and meet the requirements below,
will be considered.

Contribution requirements:

1. **Tested platforms only**. We _generally_ will only accept changes for
   platforms that are compiled and tested. This means POSIX (for Linux and
   macOS) or Windows. Very small changes will sometimes be accepted, but
   consider that more of an exception than the rule.

2. **Stable API**. We strive very hard to maintain a stable API. Changes that
   require changes for projects using leveldb _might_ be rejected without
   sufficient benefit to the project.

3. **Tests**: All changes must be accompanied by a new (or changed) test, or
   a sufficient explanation as to why a new (or changed) test is not required.

4. **Consistent Style**: This project conforms to the
   [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
   To ensure your changes are properly formatted please run:

   ```
   clang-format -i --style=file <file>
   ```

We are unlikely to accept contributions to the build configuration files, such
as `CMakeLists.txt`. We are focused on maintaining a build configuration that
allows us to test that the project works in a few supported configurations
inside Google. We are not currently interested in supporting other requirements,
such as different operating systems, compilers, or build systems.

## Submitting a Pull Request

Before any pull request will be accepted the author must first sign a
Contributor License Agreement (CLA) at https://cla.developers.google.com/.

In order to keep the commit timeline linear
[squash](https://git-scm.com/book/en/v2/Git-Tools-Rewriting-History#Squashing-Commits)
your changes down to a single commit and [rebase](https://git-scm.com/docs/git-rebase)
on google/leveldb/main. This keeps the commit timeline linear and more easily sync'ed
with the internal repository at Google. More information at GitHub's
[About Git rebase](https://help.github.com/articles/about-git-rebase/) page.

# Performance

Here is a performance report (with explanations) from the run of the
included db_bench program.  The results are somewhat noisy, but should
be enough to get a ballpark performance estimate.

## Setup

We use a database with a million entries.  Each entry has a 16 byte
key, and a 100 byte value.  Values used by the benchmark compress to
about half their original size.

    LevelDB:    version 1.1
    Date:       Sun May  1 12:11:26 2011
    CPU:        4 x Intel(R) Core(TM)2 Quad CPU    Q6600  @ 2.40GHz
    CPUCache:   4096 KB
    Keys:       16 bytes each
    Values:     100 bytes each (50 bytes after compression)
    Entries:    1000000
    Raw Size:   110.6 MB (estimated)
    File Size:  62.9 MB (estimated)

## Write performance

The "fill" benchmarks create a brand new database, in either
sequential, or random order.  The "fillsync" benchmark flushes data
from the operating system to the disk after every operation; the other
write operations leave the data sitting in the operating system buffer
cache for a while.  The "overwrite" benchmark does random writes that
update existing keys in the database.

    fillseq      :       1.765 micros/op;   62.7 MB/s
    fillsync     :     268.409 micros/op;    0.4 MB/s (10000 ops)
    fillrandom   :       2.460 micros/op;   45.0 MB/s
    overwrite    :       2.380 micros/op;   46.5 MB/s

Each "op" above corresponds to a write of a single key/value pair.
I.e., a random write benchmark goes at approximately 400,000 writes per second.

Each "fillsync" operation costs much less (0.3 millisecond)
than a disk seek (typically 10 milliseconds).  We suspect that this is
because the hard disk itself is buffering the update in its memory and
responding before the data has been written to the platter.  This may
or may not be safe based on whether or not the hard disk has enough
power to save its memory in the event of a power failure.

## Read performance

We list the performance of reading sequentially in both the forward
and reverse direction, and also the performance of a random lookup.
Note that the database created by the benchmark is quite small.
Therefore the report characterizes the performance of leveldb when the
working set fits in memory.  The cost of reading a piece of data that
is not present in the operating system buffer cache will be dominated
by the one or two disk seeks needed to fetch the data from disk.
Write performance will be mostly unaffected by whether or not the
working set fits in memory.

    readrandom  : 16.677 micros/op;  (approximately 60,000 reads per second)
    readseq     :  0.476 micros/op;  232.3 MB/s
    readreverse :  0.724 micros/op;  152.9 MB/s

LevelDB compacts its underlying storage data in the background to
improve read performance.  The results listed above were done
immediately after a lot of random writes.  The results after
compactions (which are usually triggered automatically) are better.

    readrandom  : 11.602 micros/op;  (approximately 85,000 reads per second)
    readseq     :  0.423 micros/op;  261.8 MB/s
    readreverse :  0.663 micros/op;  166.9 MB/s

Some of the high cost of reads comes from repeated decompression of blocks
read from disk.  If we supply enough cache to the leveldb so it can hold the
uncompressed blocks in memory, the read performance improves again:

    readrandom  : 9.775 micros/op;  (approximately 100,000 reads per second before compaction)
    readrandom  : 5.215 micros/op;  (approximately 190,000 reads per second after compaction)

## Repository contents

See [doc/index.md](doc/index.md) for more explanation. See
[doc/impl.md](doc/impl.md) for a brief overview of the implementation.

The public interface is in include/leveldb/*.h.  Callers should not include or
rely on the details of any other header files in this package.  Those
internal APIs may be changed without warning.

Guide to header files:

* **include/leveldb/db.h**: Main interface to the DB: Start here.

* **include/leveldb/options.h**: Control over the behavior of an entire database,
and also control over the behavior of individual reads and writes.

* **include/leveldb/comparator.h**: Abstraction for user-specified comparison function.
If you want just bytewise comparison of keys, you can use the default
comparator, but clients can write their own comparator implementations if they
want custom ordering (e.g. to handle different character encodings, etc.).

* **include/leveldb/iterator.h**: Interface for iterating over data. You can get
an iterator from a DB object.

* **include/leveldb/write_batch.h**: Interface for atomically applying multiple
updates to a database.

* **include/leveldb/slice.h**: A simple module for maintaining a pointer and a
length into some other byte array.

* **include/leveldb/status.h**: Status is returned from many of the public interfaces
and is used to report success and various kinds of errors.

* **include/leveldb/env.h**:
Abstraction of the OS environment.  A posix implementation of this interface is
in util/env_posix.cc.

* **include/leveldb/table.h, include/leveldb/table_builder.h**: Lower-level modules that most
clients probably won't use directly.
