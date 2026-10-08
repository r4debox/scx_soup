# scx_soup

In-kernel sched_ext CPU scheduler. No daemon, no user-space loop, no manual
mode toggles. The BPF program classifies every task by its own behavior and
routes it. Your radio never drops, your compile never stops.

Author: shutterspeed (Calamity Jane), 2026.
Runs on the CachyOS borefinity kernel (CONFIG_SCHED_CLASS_EXT + BTF).
Verified on an i5-8350U (4C/8T).

## 1. Specification

| Parameter            | Value                                |
|----------------------|--------------------------------------|
| Language             | BPF (clang -target bpf), userspace loader in C |
| Interface            | sched-ext struct_ops                  |
| User-space daemons   | none required                        |
| Slice, rt tier       | rt_max_slice_ns (rodata, default)    |
| Slice, batch tier    | batch_min_slice_ns (rodata, default) |
| Promote window       | promote_win_ms, rt stickiness        |
| Demote window        | demote_win_ms, batch stickiness      |
| Metrics              | EWMA per-task: slice, vsw ratio, wake latency |
| Classification       | in-BPF, every enqueue                |
| Preemption           | SCX_ENQ_PREEMPT on rt wakeups        |
| Idle selection       | scx_bpf_pick_idle_cpu within cpus_ptr |
| Kthread handling     | default path, never LOCAL_ON         |

## 2. Design principles

1. Classification is evidence-based, not guess-based.
   A zero-initialized task has slice=0, vsw=0, so the first N runs are
   recorded before promotion. A `cc` process lives ~2-4 s; it must not spend
   its whole life classified rt just because its EWMA started at zero.

2. Wake latency is the outcome, not the criterion.
   Gating rt promotion on `wake_lat < 1 ms` inverts the feedback: the task
   delayed most by load fails the gate and stays unprotected. The rt
   signature is behavioral: short on-cpu slices and frequent voluntary
   blocks (I/O wait), independent of how long it waited.

3. Never override kthread affinity.
   Per-cpu kthreads (scx watchdog, rcu, etc.) have a pinned cpu and cannot
   migrate. Inserting them to SCX_DSQ_LOCAL_ON|wrong_cpu strands them, and
   the scheduler stalls with "watchdog failed to check in" ~30 s after
   attach. Kthreads always go through the default path.

4. Preempt, don't wait for slice expiry.
   An rt wakeup that lands behind a 20 ms batch slice waits the full slice
   without SCX_ENQ_PREEMPT. With it, the rt task preempts immediately.
   Measured tail: max 21 ms -> 2.7 ms.

5. Enqueue-side LOCAL_ON is lethal; select_cpu-side is fine.
   `test_and_clear_cpu_idle` + `SCX_KICK_PREEMPT` + LOCAL_ON insert from
   enqueue froze the whole scheduler within ~1 s of attach (counters flat,
   watchdog fires at 30 s). The same pattern from select_cpu, which is where
   bpfland dispatches, runs indefinitely. Direct dispatch belongs in
   select_cpu, not enqueue.

6. Batch tier is a long slice, not a custom DSQ.
   Defining ops.dispatch changes local-rq auto-drain semantics and causes
   runnable-task stalls on this kernel. A 20 ms slice on SCX_DSQ_LOCAL gives
   the same tier separation: batch fills idle cycles, rt preempts it
   instantly.

## 3. Architecture

```
       task wakeup
            |
            v
   select_cpu (per-wakeup)
      |                          |
      | rt (class==1, in_rt)     | batch / interactive
      v                          v
   pick_idle_cpu(cpus_ptr)      scx_bpf_select_cpu_dfl
      |                          |
      v                          v
   kick + LOCAL_ON + PREEMPT    kernel default cpu
            |
            v
   enqueue (classify every wake)
      |                          |
      | rt                       | batch
      v                          v
   SCX_DSQ_LOCAL, short slice   SCX_DSQ_LOCAL, long slice
```

The classifier runs on every enqueue. Each task carries a
`struct task_metrics` in a hash map keyed by pid: slice_ewma (EWMA of on-cpu
slice), vsw_ratio_ewma (EWMA of voluntary-switch ratio), sleep_ewma (EWMA of
time asleep between enqueues; observability only, NOT used in classification).
Classify: rt if vsw > 700 (the durable I/O/stream signature, after >= 4 runs)
or slice < 1 ms from a non-rt task; batch if slice >= batch_min_run_ns (8ms)
and vsw < 250 and runs > 8. rt and batch are sticky for promote_win_ms /
demote_win_ms respectively. The batch demote gate is symmetric with the
promote gate (slice < batch_min_run_ns), not the handed-out batch slice.

## 4. Measured behavior

SDR-style probe (2000 iterations of 0.5 ms sleep) under 8x parallel
`cc -O2` compile load:

| Scheduler          | p50    | p99    | max     |
|--------------------|--------|--------|---------|
| kernel EEVDF       | (baseline) |       |         |
| scx_bpfland -m all | 598 us | 2000 us| 2996 us |
| soup (pre-PREEMPT) | 569 us | 6123 us| 21095 us|
| soup (current)     | 570 us | 641 us | 2768 us |

The 21 ms tail was the batch slice: rt wakeups waited for slice expiry. With
SCX_ENQ_PREEMPT the rt task preempts the batch task immediately; p99 drops
10x and the max goes under bpfland's.

Stability: 4 consecutive multi-minute runs under compile + SDR load, zero
watchdog events, clean detach each time. The 30 s stall that plagued early
builds is gone; see principles 3 and 5.

## 5. Build and run

```
cd ~/src/scx_soup
make
sudo ./scx_soup            # daemonizes
sudo ./scx_soup -f -s 1    # foreground, stats every second
```

Install: `make install` places the binary in /usr/local/bin and the unit
file (soup.service) in /usr/lib/systemd/system. It is not a loader
scheduler, so it does not appear in scxctl's list; the waybar menu switches
to it via scx_switch.sh soup (stop + kill loader, run standalone).

## 6. Tunables

All rodata, overridable from the loader via skel->rodata:

| Symbol                  | Effect                       |
|-------------------------|------------------------------|
| promote_win_ms          | rt stickiness window         |
| demote_win_ms           | batch stickiness window      |
| rt_max_slice_ns         | rt slice length              |
| batch_min_slice_ns      | batch slice length           |
| interactive_slice_ns    | interactive slice cap        |
| batch_min_run_ns        | batch evidence gate: slice >= this (8ms) |
| idle_prefer_cpus        | restrict tier-0 cpus (bitmask, ANDed with allowed) |

All overridable at runtime via argv, applied to rodata before load:
`scx_soup --rt-max-slice 2000000 --promote-ms 5000`

## 7. Files

- scx_soup.bpf.c   the scheduler (BPF, struct_ops)
- main.c           userspace loader (libbpf, daemonize, stats)
- minimal.bpf.c    minimal scheduler used to isolate the watchdog stall
- Makefile         clang + bpftool, no libscx
- soup.service     systemd unit (boot default, standalone)

## 8. Licensing

Copyright 2026 shutterspeed. GPL-2.0.

The sched_ext interface and the vendored headers under third_party/scx/include
are from sched-ext/scx, the reference sched_ext scheduler suite (SPDX GPL-2.0,
Copyright Meta Platforms, Tejun Heo, David Vernet). They are included verbatim
so the project builds without libscx or meson. Any sched_ext ABI knowledge in
this codebase traces to that upstream; the classification logic, tiering, and
loader are original to scx_soup.
