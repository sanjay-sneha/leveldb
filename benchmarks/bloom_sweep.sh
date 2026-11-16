#!/usr/bin/env bash
# ADDED: End-to-end Bloom filter bits-per-key sweep for db_bench.
#
# For every bits-per-key setting the database is rebuilt from scratch, because
# filters are written into SSTables when they are created; reopening an old DB
# with a different --bloom_bits silently keeps the old filters.
#
# Usage (from your build directory):
#   ../benchmarks/bloom_sweep.sh                     # defaults below
#   NUM=50000000 CACHE_TYPES="lru lfu 2q" ../benchmarks/bloom_sweep.sh
#
# Every setting can be overridden with an environment variable:
DB_BENCH=${DB_BENCH:-./db_bench}
DB_DIR=${DB_DIR:-/tmp/bloom_sweep_db}
OUT=${OUT:-bloom_sweep.csv}
NUM=${NUM:-5000000}             # keys in the DB; make the DB bigger than RAM
READS=${READS:-1000000}         # lookups per measurement
VALUE_SIZE=${VALUE_SIZE:-100}
CACHE_SIZE=${CACHE_SIZE:-8388608}  # block cache bytes (8 MB = LevelDB default)
BLOOM_BITS=${BLOOM_BITS:-"-1 2 4 6 8 10 12 16 20"}  # -1 = no filter
CACHE_TYPES=${CACHE_TYPES:-"lru"}
DISTS=${DISTS:-"uniform zipfian"}
MISSING_FRACS=${MISSING_FRACS:-"0.0 0.5 0.9"}
ZIPF_THETA=${ZIPF_THETA:-0.99}
DROP_CACHES=${DROP_CACHES:-1}   # 1 = drop the OS page cache before each read run

set -euo pipefail

if [[ ! -x "$DB_BENCH" ]]; then
  echo "db_bench not found at $DB_BENCH (set DB_BENCH=...)" >&2
  exit 1
fi

drop_os_cache() {
  [[ "$DROP_CACHES" == 1 ]] || return 0
  sync
  case "$(uname)" in
    Linux)  echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null ;;
    Darwin) sudo purge ;;
    *)      echo "warning: don't know how to drop the OS cache here" >&2 ;;
  esac
}

# Shared flags. --use_mmap=0 is required: without compression, mmap'd blocks
# never enter the block cache and every cache policy looks identical.
COMMON=(--db="$DB_DIR" --num="$NUM" --value_size="$VALUE_SIZE" --use_mmap=0)

header_written=0
: > "$OUT"

for bits in $BLOOM_BITS; do
  echo "=== bloom_bits=$bits: building DB ($NUM keys) ===" >&2
  rm -rf "$DB_DIR"
  # fillperm writes every key exactly once, so --missing_frac is exact.
  # compact settles the LSM tree so every setting is measured on the same shape.
  "$DB_BENCH" "${COMMON[@]}" --bloom_bits="$bits" \
      --benchmarks=fillperm,compact >&2

  for cache in $CACHE_TYPES; do
    for dist in $DISTS; do
      for miss in $MISSING_FRACS; do
        echo "--- bits=$bits cache=$cache dist=$dist missing=$miss ---" >&2
        drop_os_cache
        out=$("$DB_BENCH" "${COMMON[@]}" --use_existing_db=1 \
                --bloom_bits="$bits" --cache_type="$cache" \
                --cache_size="$CACHE_SIZE" --reads="$READS" \
                --read_dist="$dist" --zipf_theta="$ZIPF_THETA" \
                --missing_frac="$miss" --histogram=1 --bloom_stats=1 \
                --benchmarks=readmix 2>/dev/null)
        if [[ $header_written == 0 ]]; then
          echo "$out" | grep '^RESULT,benchmark' | sed 's/^RESULT,//' >> "$OUT"
          header_written=1
        fi
        echo "$out" | grep '^RESULT,readmix' | sed 's/^RESULT,//' >> "$OUT"
      done
    done
  done
done

rm -rf "$DB_DIR"
echo "Wrote $(($(wc -l < "$OUT") - 1)) rows to $OUT" >&2
