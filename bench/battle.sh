#!/bin/bash
# battle.sh - soup vs the installed sched-ext field. 6-way sweep.
# Each scheduler gets: sched_bench (latency+throughput), audio_probe (stutter),
# and the 4-phase stress card. Results appended to out/battle_results.txt.
# soup is standalone (switch script special-cases it); the rest run via the
# loader at mode 0 (Auto) so they get their shipped defaults.
# Self-contained: anchors on this script's dir (repo bench/), outputs to out/.
set -uo pipefail
BENCH="$(cd "$(dirname "$0")" && pwd)"
OUT="$BENCH/out"
mkdir -p "$OUT"
RES="$OUT/battle_results.txt"
: > "$RES"
echo "battle start $(date '+%F %T') | host kernel $(uname -r)" | tee -a "$RES"
echo "state before: $(cat /sys/kernel/sched_ext/state 2>/dev/null)" | tee -a "$RES"

# roster: soup is the home fighter (boot default, standalone); rivals via
# loader (mode 0 = Auto). Loader names are the full scx_<name> form.
ROSTER=(soup scx_bpfland scx_rusty scx_lavd scx_p2dq scx_mlfq)

ensure_state() { # $1 = expected (enabled|disabled), polls up to 15s
  local want="$1" i
  for i in $(seq 1 30); do
    [ "$(cat /sys/kernel/sched_ext/state 2>/dev/null)" = "$want" ] && return 0
    sleep 0.5
  done
  echo "WARN: state still != $want after 15s ($(cat /sys/kernel/sched_ext/state 2>/dev/null))" | tee -a "$RES"
  return 1
}

run_bout() {
  local sched="$1"
  echo "============================================================" | tee -a "$RES"
  echo "[battle] bout: $sched at $(date +%T)" | tee -a "$RES"
  # swap to the scheduler
  if [ "$sched" = "soup" ]; then
    ~/.local/bin/scx_switch.sh soup 2>&1 | tail -2 | tee -a "$RES"
  else
    ~/.local/bin/scx_switch.sh "$sched" 0 2>&1 | tail -2 | tee -a "$RES"
  fi
  ensure_state enabled || { echo "[battle] SKIP $sched (no engage)" | tee -a "$RES"; return 1; }
  sleep 3   # let the scheduler settle / warm caches
  echo "[battle] engaged: $sched | sched_ext=$(cat /sys/kernel/sched_ext/state 2>/dev/null)" | tee -a "$RES"

  # 1. latency + compile throughput
  echo "[battle] -- sched_bench $sched --" | tee -a "$RES"
  "$BENCH/sched_bench.sh" "$sched" 2>&1 | tee -a "$RES"

  # 2. audio stutter probe (the real complaint: 10ms pipewire cycle, 1ms work)
  echo "[battle] -- audio_probe $sched (10ms/1ms) --" | tee -a "$RES"
  "$BENCH/audio_probe.sh" "${sched}_w3" 10 1 2>&1 | tee -a "$RES"

  # 3. 4-phase stress card
  echo "[battle] -- stress $sched --" | tee -a "$RES"
  "$BENCH/stress_soup.sh" 2>&1 | tee -a "$RES"

  # stall check after every bout
  echo "[battle] -- dmesg stall check --" | tee -a "$RES"
  sudo dmesg | grep -iE "watchdog failed|hung task|sched_ext.*(BUG|stall)" | tail -3 | tee -a "$RES" || echo "clean" | tee -a "$RES"
}

for s in "${ROSTER[@]}"; do
  run_bout "$s"
done

# home again: soup is the boot default
echo "============================================================" | tee -a "$RES"
echo "[battle] restore: back to soup" | tee -a "$RES"
~/.local/bin/scx_switch.sh soup 2>&1 | tail -2 | tee -a "$RES"
ensure_state enabled || echo "[battle] WARN: restore failed" | tee -a "$RES"
echo "[battle] end $(date '+%F %T') | state=$(cat /sys/kernel/sched_ext/state 2>/dev/null)" | tee -a "$RES"
echo "RESULTS: $RES"
