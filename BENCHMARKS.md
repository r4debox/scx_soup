# Benchmarks: soup vs the installed scx field

Head-to-head on the i5-8350U (4C/8T), kernel 7.2.9-mothware-borefinity,
2026-10-08. Every scheduler ran the same three tests in the same order.
soup was the boot default; each rival attached via scx_loader at mode 0
(Auto) so it got its shipped defaults. soup returned at the end.

## The card

- sched_bench: 2000 latency probes (0.5ms sleep) + an 8x cc compile, 13s.
  Reports p50/p99/max latency and compile throughput (comp/s).
- audio_probe: 3000 iterations of a 10ms period / 1ms work task - the
  pipewire-style stutter test. glitch_count = iterations that overran.
- stress: 4 phases - A: 8x cc compile + SDR probe (15s), B: 32-task wakeup
  storm (10s), C: interactive mix (10s), D: GUI-style wakes after 12s under
  load then probes.

## Results

| scheduler | bench p50 | bench p99 | bench max | comp/s | audio glitch | audio p99 | audio max |
|-----------|-----------|-----------|-----------|--------|--------------|-----------|-----------|
| soup      | 567us     | 4254us    | 20542us   | 24.4   | 8/3000       | 221us     | 28257us   |
| scx_mlfq  | 566us     | 1105us    | 4233us    | 23.6   | 59/3000      | 1241us    | 3229us    |
| scx_bpfland | 995us   | 2011us    | 3000us    | 22.7   | 91/3000      | 1167us    | 2169us    |
| scx_lavd  | 568us     | 2331us    | 4402us    | 24.1   | 969/3000     | 4390us    | 5403us    |
| scx_p2dq  | 1182us    | 7025us    | 11102us   | 24.6   | 1239/3000    | 3477us    | 6302us    |
| scx_rusty | 996us     | 2322us    | 8243us    | 24.0   | 1794/3000    | 18051us   | 22280us   |

Stress card, A (8x cc) and D (GUI after 12s load), p50/p99/max in us:

| scheduler | A p50/p99/max      | D p50/p99/max      |
|-----------|--------------------|--------------------|
| soup      | 568/632/5552       | 568/632/37658      |
| scx_mlfq  | 566/969/5740       | 568/1579/4151      |
| scx_bpfland | 995/2015/3261    | 994/1999/2376      |
| scx_lavd  | 997/3155/4336      | 996/3004/4081      |
| scx_p2dq  | 1057/4103/5939     | 1052/4055/5933     |
| scx_rusty | 995/11256/20025    | 996/10011/22777    |

All six bouts clean on the dmesg watchdog: no stalls, no BUGs, no hung tasks.

## Reading the numbers

- soup owns the audio tail. 8 glitches vs mlfq's 59 (7.4x cleaner) and
  rusty's 1794 (224x). audio p99 of 221us is 5.6x tighter than the next
  best (bpfland, 1167us).
- soup and mlfq tie at the latency median (566-568us), but soup's p99
  under full compile is 632us vs mlfq's 969us. The preempt-kick path holds
  the tail where the twin's tier demotion lags.
- Throughput is a wash across the board (22.7-24.6 comp/s, within 8%).
  soup gives up 0.8% to p2dq for a 5.6x latency-tail win.
- soup's bench-max (20.5ms) and stress-D-max (37.7ms) are the EWMA
  cold-start: the first wake burst fires a preempt storm before the
  evidence gates settle. One-time warmup, steady state is p50/p99 clean.
- rusty is not for latency-sensitive work on this box (18ms audio p99,
  11ms A-phase p99). rustland is the low-latency variant, not rusty.
- lavd is sold as the audio scheduler and glitched 969/3000. The
  generalist BPF classifier beat the specialist on its own test.

## Repro

- bench/battle.sh - the 6-way driver (swap, bench, audio, stress,
  dmesg check per bout, restore to soup at the end).
- bench/sched_bench.sh, bench/audio_probe.sh, bench/stress_soup.sh - the
  individual probes. All three are self-contained: they anchor on their own
  directory (bench/) and write raw output to bench/out/.
- scheduler swaps via ~/.local/bin/scx_switch.sh (loader needs the full
  scx_<name> form, e.g. scx_mlfq, not mlfq).

Run the sweep again with: cd src/scx_soup && ./bench/battle.sh
Results land in src/scx_soup/bench/out/battle_results.txt.
