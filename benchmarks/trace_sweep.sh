#!/usr/bin/env bash
# Replay a Twitter cache trace against every cache policy.
#
# For each bloom_bits setting, the trace's key set is loaded once (tracefill)
# into a base DB. Every replay then starts from a fresh copy of that base,
# because the replay itself writes and deletes keys. Each replay runs under a
# cgroup memory cap so the DB is larger than the memory available to it, and
# the OS page cache is dropped first so every run starts cold.
#
# Usage (from the build directory):
#   TRACE=~/data/twitter52.csv ../benchmarks/trace_sweep.sh
#
# Settings (override with environment variables):
DB_BENCH=${DB_BENCH:-./db_bench}
TRACE=${TRACE:?set TRACE=/path/to/trace.csv}
WORK=${WORK:-$HOME/trace_sweep_db}
OUT=${OUT:-trace_sweep.csv}
TRACE_OPS=${TRACE_OPS:--1}          # trace lines to replay (-1 = all)
CACHE_SIZE=${CACHE_SIZE:-8388608}   # block cache bytes (8 MB, LevelDB default)
BLOOM_BITS=${BLOOM_BITS:-"-1 10"}   # -1 = no filter
CACHE_TYPES=${CACHE_TYPES:-"lru lfu 2q arc lecar"}
REPEATS=${REPEATS:-1}               # runs per setting
MEMCAP=${MEMCAP:-256M}              # cgroup memory cap; "none" to disable
DROP_CACHES=${DROP_CACHES:-1}       # 1 = drop the OS page cache before runs

set -euo pipefail

if [[ ! -x "$DB_BENCH" ]]; then
  echo "db_bench not found at $DB_BENCH (set DB_BENCH=...)" >&2
  exit 1
fi
DB_BENCH=$(realpath "$DB_BENCH")
TRACE=$(realpath "$TRACE")

drop_os_cache() {
  [[ "$DROP_CACHES" == 1 ]] || return 0
  sync
  echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

# Runs db_bench under the memory cap, as the current user.
run_capped() {
  if [[ "$MEMCAP" == none ]]; then
    "$DB_BENCH" "$@"
  else
    sudo systemd-run --scope --quiet --uid="$(id -u)" --gid="$(id -g)" \
        -p MemoryMax="$MEMCAP" -p MemorySwapMax=0 "$DB_BENCH" "$@"
  fi
}

COMMON=(--trace="$TRACE" --trace_ops="$TRACE_OPS" --use_mmap=0)

header_written=0
: > "$OUT"
mkdir -p "$WORK"

for bits in $BLOOM_BITS; do
  echo "=== bloom_bits=$bits: loading trace keys into base DB ===" >&2
  rm -rf "$WORK/base"
  "$DB_BENCH" "${COMMON[@]}" --db="$WORK/base" --bloom_bits="$bits" \
      --benchmarks=tracefill,compact >&2

  for rep in $(seq 1 "$REPEATS"); do
    for cache in $CACHE_TYPES; do
      echo "--- bits=$bits cache=$cache run=$rep ---" >&2
      rm -rf "$WORK/run"
      cp -r "$WORK/base" "$WORK/run"
      drop_os_cache
      out=$(run_capped "${COMMON[@]}" --db="$WORK/run" --use_existing_db=1 \
              --bloom_bits="$bits" --cache_type="$cache" \
              --cache_size="$CACHE_SIZE" --histogram=1 --bloom_stats=1 \
              --benchmarks=tracereplay 2>/dev/null)
      echo "$out" | grep '^tracereplay' >&2 || true
      if [[ $header_written == 0 ]]; then
        echo "$out" | grep '^RESULT,benchmark' | sed 's/^RESULT,//; s/$/,run/' \
            >> "$OUT"
        header_written=1
      fi
      echo "$out" | grep '^RESULT,tracereplay' |
          sed "s/^RESULT,//; s/\$/,$rep/" >> "$OUT"
    done
  done
done

rm -rf "$WORK/run"
echo "Wrote $(($(wc -l < "$OUT") - 1)) rows to $OUT" >&2