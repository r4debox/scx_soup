#!/bin/bash
# sched_bench.sh <label> - measure rt latency + compile throughput under a scheduler
# usage: sched_bench.sh <label>
#  - runs 8 parallel cc -O2 loops for a fixed 13s window (counts completed compiles)
#  - concurrently runs the SDR-style 0.5ms-sleep latency probe
#  - reports: label | p50/p99/max latency | compiles completed | avg compile/s
# Self-contained: anchors on this script's dir (repo bench/), outputs to out/.
set -u
LABEL=${1:?label}
BENCH="$(cd "$(dirname "$0")" && pwd)"
OUT="$BENCH/out"
mkdir -p "$OUT"
cd "$BENCH" || exit 1

# compile load: 8 workers, each compiles big.c repeatedly for ~13s, count total
COMPILE_COUNT_FILE="$OUT/count_${LABEL}.txt"
rm -f "$COMPILE_COUNT_FILE"
for w in 0 1 2 3 4 5 6 7; do
  cp big.c big_${w}.c
done

run_worker() {
  local w=$1 n=0
  local end=$((SECONDS + 13))
  while [ $SECONDS -lt $end ]; do
    cc -O2 -c big_${w}.c -o /dev/null 2>/dev/null && n=$((n+1))
  done
  echo $n >> "$COMPILE_COUNT_FILE"
}
for w in 0 1 2 3 4 5 6 7; do run_worker $w & done

# rt probe concurrent
python3 - > "$OUT/lat_${LABEL}.raw" 2>/dev/null <<'PYEOF'
import time
N = 2000
lats = []
for i in range(N):
    t0 = time.perf_counter_ns()
    time.sleep(0.0005)
    t1 = time.perf_counter_ns()
    lats.append((t1 - t0) // 1000)
lats.sort()
print(f"{lats[len(lats)//2]} {lats[int(len(lats)*0.99)]} {lats[-1]} {sum(lats)//len(lats)}")
PYEOF

wait
# aggregate
TOTAL=$(paste -sd+ "$COMPILE_COUNT_FILE" 2>/dev/null | bc 2>/dev/null || awk '{s+=$1} END{print s}' "$COMPILE_COUNT_FILE")
read P50 P99 MX AVG < "$OUT/lat_${LABEL}.raw"
echo "$LABEL: latency p50=${P50}us p99=${P99}us max=${MX}us avg=${AVG}us | compiles=${TOTAL} (13s) = $(echo "scale=1; $TOTAL/13" | bc) comp/s"
for w in 0 1 2 3 4 5 6 7; do rm -f big_${w}.c; done
