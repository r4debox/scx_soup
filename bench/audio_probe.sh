#!/bin/bash
# audio_probe.sh - simulate a pipewire-style audio cycle under compile load
# wakes every PERIOD_MS (default 10), does WORK_MS of busywork (default 1), measures
# cycle overrun = actual period - nominal. A stutter = one cycle arriving > period.
# usage: audio_probe.sh <label> [period_ms] [work_ms]
# Self-contained: anchors on this script's dir (repo bench/), outputs to out/.
set -u
LABEL=${1:?label}
PERIOD=${2:-10}
WORK=${3:-1}
BENCH="$(cd "$(dirname "$0")" && pwd)"
OUT="$BENCH/out"
mkdir -p "$OUT"
cd "$BENCH" || exit 1
for w in 0 1 2 3 4 5 6 7; do cp big.c big_${w}.c; done
( end=$((SECONDS+30)); while [ $SECONDS -lt $end ]; do
    for w in 0 1 2 3 4 5 6 7; do cc -O2 -c big_${w}.c -o /dev/null 2>/dev/null &
    done
    wait
  done ) >/dev/null 2>&1 &
CPID=$!
sleep 1
python3 - > "$OUT/audio_${LABEL}.txt" 2>/dev/null <<PYEOF
import time, statistics, sys
period = ${PERIOD} / 1000.0
work = ${WORK} / 1000.0
N = 3000  # 30s at 10ms
next_t = time.perf_counter()
overruns = []
for i in range(N):
    next_t += period
    # do the work: ~1ms of busy CPU
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < work:
        pass
    # sleep until next period
    now = time.perf_counter()
    target = next_t
    if now < target:
        time.sleep(target - now)
    overrun = (time.perf_counter() - target) * 1000  # ms late
    overruns.append(overrun)
# report: overruns >1ms (audible glitch) and p50/p99/max of the late time
overruns.sort()
n_glitch = sum(1 for o in overruns if o > 1.0)
print(f"glitch_count={n_glitch}/{N} p50={overruns[len(overruns)//2]*1000:.0f}us p99={overruns[int(len(overruns)*0.99)]*1000:.0f}us max={overruns[-1]*1000:.0f}us")
PYEOF
kill $CPID 2>/dev/null; wait 2>/dev/null
for w in 0 1 2 3 4 5 6 7; do rm -f big_${w}.c; done
cat "$OUT/audio_${LABEL}.txt"
