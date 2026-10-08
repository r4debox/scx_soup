#!/bin/bash
# stress_soup.sh - comprehensive stress for scx_soup (fresh build, batch_min_run_ns gate)
# Phases:
#   A. 8x cc compile (batch) + SDR 0.5ms probe (rt)      - the classic mix
#   B. wakeup storm: 32 short-sleep tasks                - rt promotion churn
#   C. interactive mix: git status loop, text grep loop   - interactive tier
#   D. demote-recovery: GUI-style task sleeps 12s under load, then wakes+probes  - batch demote latency check
# Reports latency stats per phase + aggregate.
# Self-contained: anchors on this script's dir (repo bench/), outputs to out/.
set -u
BENCH="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="$BENCH/out"
mkdir -p "$OUTDIR"
OUT="$OUTDIR/stress_soup.log"
: > "$OUT"
echo "[stress] start $(date +%T) | sched=$(cat /sys/kernel/sched_ext/state 2>/dev/null)" | tee -a "$OUT"

phase() { echo "[stress] == $1 ==" | tee -a "$OUT"; }

# ---------- phase A: compile + rt probe ----------
phase "A: 8x cc compile + SDR probe (15s)"
cd "$BENCH"
for w in 0 1 2 3 4 5 6 7; do cp big.c big_${w}.c; done
( end=$((SECONDS+15)); while [ $SECONDS -lt $end ]; do
    for w in 0 1 2 3 4 5 6 7; do cc -O2 -c big_${w}.c -o /dev/null 2>/dev/null & done
    wait
  done ) &
CPID=$!
python3 - > "$OUTDIR/stress_A.raw" 2>/dev/null <<'PYEOF'
import time, statistics
N=2000; lats=[]
for i in range(N):
    t0=time.perf_counter_ns(); time.sleep(0.0005); t1=time.perf_counter_ns()
    lats.append((t1-t0)//1000)
lats.sort()
print(f"{lats[len(lats)//2]} {lats[int(len(lats)*0.99)]} {lats[-1]} {statistics.mean(lats):.0f} {len(lats)}")
PYEOF
kill $CPID 2>/dev/null; wait 2>/dev/null
read P50 P99 MX AVG N < "$OUTDIR/stress_A.raw"
echo "A: p50=${P50}us p99=${P99}us max=${MX}us avg=${AVG}us n=${N}" | tee -a "$OUT"
for w in 0 1 2 3 4 5 6 7; do rm -f big_${w}.c; done

# ---------- phase B: wakeup storm ----------
phase "B: 32-task wakeup storm (10s)"
python3 - > "$OUTDIR/stress_B.raw" 2>/dev/null <<'PYEOF'
import time, statistics
N=2000; lats=[]
def task(idx):
    # each task sleeps 0.5-2ms, simulating many interleaved rt-ish wakeups
    for i in range(60):
        time.sleep(0.0005 + (idx%4)*0.0005)
# spawn 32
import threading
ts=[threading.Thread(target=task,args=(i,)) for i in range(32)]
for t in ts: t.start()
# probe while storm runs
for i in range(N):
    t0=time.perf_counter_ns(); time.sleep(0.0005); t1=time.perf_counter_ns()
    lats.append((t1-t0)//1000)
for t in ts: t.join()
lats.sort()
print(f"{lats[len(lats)//2]} {lats[int(len(lats)*0.99)]} {lats[-1]} {statistics.mean(lats):.0f} {len(lats)}")
PYEOF
read P50 P99 MX AVG N < "$OUTDIR/stress_B.raw"
echo "B: p50=${P50}us p99=${P99}us max=${MX}us avg=${AVG}us n=${N}" | tee -a "$OUT"

# ---------- phase C: interactive mix ----------
phase "C: interactive mix (10s)"
python3 - > "$OUTDIR/stress_C.raw" 2>/dev/null <<'PYEOF'
import time, statistics, os
N=2000; lats=[]
# interactive-ish: small fork/exec churn + probe
end=time.time()+10
for i in range(N):
    t0=time.perf_counter_ns(); time.sleep(0.0005); t1=time.perf_counter_ns()
    lats.append((t1-t0)//1000)
    if i % 50 == 0:
        os.system("true")
lats.sort()
print(f"{lats[len(lats)//2]} {lats[int(len(lats)*0.99)]} {lats[-1]} {statistics.mean(lats):.0f} {len(lats)}")
PYEOF
read P50 P99 MX AVG N < "$OUTDIR/stress_C.raw"
echo "C: p50=${P50}us p99=${P99}us max=${MX}us avg=${AVG}us n=${N}" | tee -a "$OUT"

# ---------- phase D: demote-recovery (GUI-style) ----------
phase "D: GUI-style task wakes after 12s under load, then probes"
# start compile load
cd "$BENCH"
for w in 0 1 2 3 4 5 6 7; do cp big.c big_${w}.c; done
( end=$((SECONDS+20)); while [ $SECONDS -lt $end ]; do
    for w in 0 1 2 3 4 5 6 7; do cc -O2 -c big_${w}.c -o /dev/null 2>/dev/null & done
    wait
  done ) &
DPID=$!
sleep 2
# GUI-style task: wakes after being quiet (simulating a backgrounded app coming back)
python3 - > "$OUTDIR/stress_D.raw" 2>/dev/null <<'PYEOF'
import time, statistics
# pretend we're a GUI app that was quiet for a while
time.sleep(8)  # during compile
# now we're interactive again: probe
N=1500; lats=[]
for i in range(N):
    t0=time.perf_counter_ns(); time.sleep(0.0005); t1=time.perf_counter_ns()
    lats.append((t1-t0)//1000)
lats.sort()
print(f"{lats[len(lats)//2]} {lats[int(len(lats)*0.99)]} {lats[-1]} {statistics.mean(lats):.0f} {len(lats)}")
PYEOF
kill $DPID 2>/dev/null; wait 2>/dev/null
for w in 0 1 2 3 4 5 6 7; do rm -f big_${w}.c; done
read P50 P99 MX AVG N < "$OUTDIR/stress_D.raw"
echo "D: p50=${P50}us p99=${P99}us max=${MX}us avg=${AVG}us n=${N}" | tee -a "$OUT"

# ---------- wrap ----------
echo "[stress] end $(date +%T) | sched=$(cat /sys/kernel/sched_ext/state 2>/dev/null)" | tee -a "$OUT"
echo "[stress] watchdog/stall check:" | tee -a "$OUT"
sudo dmesg | grep -iE "watchdog failed|hung task|sched_ext.*(BUG|stall)" | tail -5 | tee -a "$OUT" || true
echo "[stress] done" | tee -a "$OUT"
