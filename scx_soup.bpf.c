/*
 * scx_soup.bpf.c
 *
 * An in-kernel sched_ext scheduler that classifies tasks live and forwards
 * them into one of three tiers with ZERO userspace involvement:
 *
 *   TIER 0  low-latency streams (SDR, audio, input): dispatched to a target
 *           cpu's local DSQ from select_cpu() with SCX_ENQ_PREEMPT. A
 *           dedicated idle core is preferred; slice is short (4ms) so
 *           wakeups preempt fast.
 *
 *   TIER 1  interactive / default: normal local-DSQ behavior with a modest
 *           slice.
 *
 *   TIER 2  batch absorption (make, cargo, ninja, clang): long slice (20ms)
 *           on the local DSQ. No custom DSQ, no ops.dispatch: on this kernel
 *           that combination changes local-rq auto-drain semantics and
 *           causes runnable-task stalls. Long-slice-on-local gives the same
 *           tier separation: batch fills idle cycles, rt preempts it
 *           instantly.
 *
 * Everything is decided in-kernel. The BPF program owns the tunables
 * (rodata), the classifier, and the queue plumbing. The only "userspace"
 * piece is a libbpf loader that attaches the program and prints per-cpu
 * stats; no daemon is required for scheduling decisions.
 *
 * ABI: this targets Linux 7.2 with the cid-based sched_ext ops
 * (scx_bpf_dsq_insert___v2, select_cpu_and, local DSQ via
 * SCX_DSQ_LOCAL_ON at dispatch/insert time). It deliberately does NOT
 * use libscx or the meson/rust build; the scheduler must be a standalone
 * BPF object + loader.
 *
 * Copyright 2026 shutterspeed, GPL-2.0
 *
 * sched_ext interface and the vendored headers under third_party/ are
 * from sched-ext/scx (GPL-2.0, Copyright Meta Platforms, Tejun Heo,
 * David Vernet). See README section 8.
 */
#include "common.bpf.h"
#include "user_exit_info.bpf.h"

/*
 * CRITICAL: the vendored enums.autogen.bpf.h defines SCX_DSQ_* / SCX_ENQ_*
 * as `const volatile u64 __SCX_*` RODATA vars that default to ZERO unless a
 * libscx-style loader populates them from kernel BTF. My plain libbpf loader
 * doesn't, so at runtime SCX_DSQ_GLOBAL etc. are 0 -> the kernel rejects
 * DSQ 0x0 and kills the scheduler ("invalid DSQ ID 0x0000000000000000").
 *
 * These are STABLE kernel ABI constants (from vmlinux.h); hardcode them so
 * the scheduler works without enum rodata population.
 */
#undef SCX_DSQ_FLAG_BUILTIN
#undef SCX_DSQ_FLAG_LOCAL_ON
#undef SCX_DSQ_INVALID
#undef SCX_DSQ_GLOBAL
#undef SCX_DSQ_LOCAL
#undef SCX_DSQ_BYPASS
#undef SCX_DSQ_LOCAL_ON
#undef SCX_DSQ_LOCAL_CPU_MASK
#define SCX_DSQ_FLAG_BUILTIN    0x8000000000000000ULL
#define SCX_DSQ_FLAG_LOCAL_ON   0x4000000000000000ULL
#define SCX_DSQ_INVALID         0x8000000000000000ULL
#define SCX_DSQ_GLOBAL          0x8000000000000001ULL
#define SCX_DSQ_LOCAL           0x8000000000000002ULL
#define SCX_DSQ_BYPASS          0x8000000000000003ULL
#define SCX_DSQ_LOCAL_ON        0xC000000000000000ULL
#define SCX_DSQ_LOCAL_CPU_MASK  0xFFFFFFFFULL

#undef SCX_ENQ_WAKEUP
#undef SCX_ENQ_HEAD
#undef SCX_ENQ_CPU_SELECTED
#undef SCX_ENQ_PREEMPT
#undef SCX_ENQ_IMMED
#undef SCX_ENQ_REENQ
#undef SCX_ENQ_LAST
#define SCX_ENQ_WAKEUP          1ULL
#define SCX_ENQ_HEAD            0x10000ULL
#define SCX_ENQ_CPU_SELECTED    0x100000ULL
#define SCX_ENQ_PREEMPT         0x100000000ULL
#define SCX_ENQ_IMMED           0x200000000ULL
#define SCX_ENQ_REENQ           0x10000000000ULL
#define SCX_ENQ_LAST            0x20000000000ULL

char LICENSE[] SEC("license") = "GPL";

/* ======================= tunables (rodata) ======================= */

const volatile u64 rt_max_slice_ns  = 4000000;   /* 4ms: keep preemption fast */
const volatile u64 batch_min_slice_ns = 20000000; /* 20ms: long uninterrupted run */
const volatile u64 batch_min_run_ns   = 8000000;  /* batch evidence: slice >= 8ms */
const volatile u64 interactive_slice_ns = 4000000; /* 4ms: cap-hit is a distinct signal */
const volatile u64 promote_win_ms = 3000;   /* re-eval window for promotion */
const volatile u64 demote_win_ms  = 8000;   /* re-eval window for demotion */
const volatile s32 idle_prefer_cpus = 0;    /* bitmask hint for tier-0 cores; 0 = auto */
const volatile bool verbose = false;

/* ======================= telemetry maps ======================= */

struct task_metrics {
    /*
     * EWMA of sleep time (ns), updated in enqueue. last_enq_ns is stamped
     * in stopping(), so the enqueue delta is "time asleep + wake delay",
     * NOT wake-to-run latency. Kept for observability only; classification
     * deliberately does NOT gate on it (wake latency is the outcome we are
     * protecting, gating on it inverts the feedback).
     */
    u64 sleep_ewma;
    /* EWMA of last observed run slice (ns), updated in stopping() */
    u64 slice_ewma;
    /* EWMA of voluntary-switch ratio (0..1000, per mille) */
    u64 vsw_ratio_ewma;
    u64 last_start;        /* last running() timestamp */
    u64 last_stop;         /* last stopping() timestamp */
    u64 last_enq_ns;       /* last enqueue time (sleep time sample origin) */
    u64 last_classify_ns;  /* last time we re-classified */
    u64 runs;              /* total completed slices */
    u64 vol_switches;      /* count of voluntary switches */
    u64 no_vol_switches;   /* count of involuntary switches */
    u32 prio;              /* sched priority snapshot */
    u8  klass;             /* 0=unknown 1=rt_stream 2=interactive 3=batch */
    u8  in_rt;             /* sticky flag, cleared after demote window */
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 32768);
    __type(key, u32);
    __type(value, struct task_metrics);
} task_metrics_map SEC(".maps");

/* per-cpu stats for observability */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} rt_wake_count SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} rt_direct_count SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} batch_absorbed_count SEC(".maps");

/* ---- debug instrumentation: per-op call counters ---- */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} dbg_init_task_calls SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} dbg_running_calls SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} dbg_stopping_calls SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} dbg_enqueue_calls SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} dbg_select_cpu_calls SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} dbg_classify_rt SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} dbg_classify_batch SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} dbg_classify_interactive SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} dbg_metrics_miss SEC(".maps");

/* ======================= helpers ======================= */

static __always_inline struct task_metrics *get_metrics(u32 tid)
{
    return bpf_map_lookup_elem(&task_metrics_map, &tid);
}

/* verbose-gated trace: only fires when --verbose is set (rodata) */
static __always_inline void vdbg(const char *msg, u64 a, u64 b)
{
    if (verbose)
        bpf_printk("%s %llu %llu", msg, a, b);
}

/* Conservative EWMA: fraction = 1 << shift (hysteresis via promotion window) */
static __always_inline u64 ewma_update(u64 old, u64 sample, u64 shift)
{
    if (!old)
        return sample;
    /* (old * (2^shift - 1) + sample) >> shift */
    return (old * ((1ULL << shift) - 1) + sample) >> shift;
}

/* Bump a percpu counter map (maps are anonymous structs, so this must be
 * a macro: we can't pass the map through a typed helper). */
#define BUMP_PERCPU(__map)							\
	do {								\
		u32 __k = 0;						\
		u64 *__v = bpf_map_lookup_elem(&(__map), &__k);		\
		if (__v)						\
			__sync_fetch_and_add(__v, 1);			\
	} while (0)

static __always_inline u64 now_ns(void)
{
    return bpf_ktime_get_ns();
}

/*
 * Classify a task from its metrics. Called from select_cpu/enqueue and from
 * the re-eval path.
 *
 * Signals (EWMAs in struct task_metrics):
 *  - sleep_ewma: time asleep between enqueues (observability only; NOT used
 *    in classification. See the struct comment on why gating on it would
 *    invert the feedback).
 *  - slice_ewma: actual run slice. A task that consistently SATURATES the
 *    interactive slice cap (4ms) and almost never yields voluntarily is
 *    compute-bound: promote to batch (longer slices, background DSQ).
 *  - vsw_ratio_ewma: per-mille voluntary-switch ratio. High = I/O/stream,
 *    low = compute.
 *
 * Thresholds:
 *  RT stream: vsw > 700 (durable I/O/stream signature). The slice < 1ms
 *             gate applies ONLY to tasks not already rt: once a task gets a
 *             rt slice (4ms), the slice EWMA rises and slice < 1ms can never
 *             be true again, so gating stay-rt on slice would churn tasks
 *             out every promote_win. vsw is the only durable rt signal.
 *  Batch:     slice >= 3/4 of the interactive cap (saturating) AND vsw < 250
 *             AND runs > 8 (enough history)
 *  Otherwise interactive.
 *
 * Hysteresis: once rt_stream, stay rt_stream for promote_win_ms; once batch,
 * need vsw to rise / slice to fall for demote_win_ms before re-eval.
 */
static __always_inline s32 classify_task(struct task_metrics *m, u64 now)
{
    u64 slice    = m->slice_ewma;
    u64 vsw      = m->vsw_ratio_ewma;

    /*
     * rt stickiness: once a stream, stay rt for the FULL demote window.
     * Using promote_win (3s) here lets a single vsw dip at the 3s re-eval
     * demote a real stream, and then it cannot recover: the rt slice (4ms)
     * raises slice_ewma so the slice < 1ms re-promote gate (line 303) can
     * never fire, and it rides the interactive tier behind batch tasks
     * until vsw recovers. Asymmetric with batch (which sticks 8s) and
     * wrong for latency-sensitive tasks.
     */
    if (m->klass == 1 && now - m->last_classify_ns < demote_win_ms * 1000000ULL)
        return 1; /* stickiness for rt */

    /*
     * rt requires EVIDENCE: a zero-initialized task would trivially satisfy
     * slice < 1ms and get stuck rt for the whole sticky window. Require a
     * few completed runs so the EWMA means something before promoting.
     *
     * vsw > 700 is the durable stream signature (frequent voluntary blocks,
     * I/O wait). slice < 1ms is only admitted from a NON-rt task: once rt
     * hands out a rt_max_slice (4ms), slice_ewma rises and the slice gate
     * would self-erode, so stay-rt must rest on vsw alone.
     */
    if (m->runs >= 4 && vsw > 700)
        return 1;

    /*
     * Slice gate: a task that ACTUALLY ran a sub-ms slice is a micro-slicer
     * by evidence, not by zero-init. runs >= 4 would force a fresh SDR/GUI
     * task to burn 4 interactive slices (up to 20ms each under batch load)
     * before it can preempt: a 26ms cold-start stall measured under 8x cc.
     * The zero-init worry (slice_ewma == 0 promoting a never-run task) is
     * harmless: it gets one 4ms rt slice, then classifies on real evidence,
     * and the demote_win stickiness bounds any over-promotion. Never-run
     * tasks also cannot hit this branch more than once.
     */
    if (m->klass != 1 && slice < 1000000ULL)
        return 1;

    if (m->klass == 3 && now - m->last_classify_ns < demote_win_ms * 1000000ULL)
        return 3; /* stickiness for batch */

    if (slice >= batch_min_run_ns && vsw < 250 && m->runs > 8)
        return 3;

    /*
     * Batch demote: symmetric with the promote gate (slice < batch_min_run_ns
     * = 8ms), not the batch slice. batch_min_slice_ns is the SLICE HANDED OUT
     * to batch tasks (20ms), so comparing slice < 20ms would be dead weight:
     * every batch task runs ~20ms slices and the promote check already
     * returns 3 for anything in [8ms, 20ms). Effective demote = the task
     * stopped running long enough to be batch.
     */
    if (m->klass == 3 && slice < batch_min_run_ns &&
        now - m->last_classify_ns > demote_win_ms * 1000000ULL)
        return 2; /* demoted back to interactive */

    return 2;
}

static __always_inline void set_klass(struct task_metrics *m, s32 k, u64 now)
{
    m->klass = k;
    if (k == 1)
        m->in_rt = 1;      /* sticky: promoted by the classifier */
    else if (k != 1 && m->in_rt)
        m->in_rt = 0;      /* demoted: no longer rt */
    m->last_classify_ns = now;
}

/*
 * Pick a target cpu for a tier-0 task. Prefer an idle core in the process's
 * allowed cpumask; if idle_prefer_cpus (bitmask) is set, restrict selection
 * to (hint & allowed). Returns -1 when no idle cpu is available; the caller
 * falls back to prev_cpu / current.
 */
static __always_inline s32 pick_tier0_cpu(struct task_struct *p, s32 prev_cpu)
{
    s32 cpu;

    if (idle_prefer_cpus) {
        /* explicit user hint: restrict to (hint & allowed), idle preferred */
        struct bpf_cpumask *mask = bpf_cpumask_create();
        u32 nr = scx_bpf_nr_cpu_ids();
        if (!mask)
            return -1;
        for (u32 i = 0; i < nr; i++) {
            if ((idle_prefer_cpus & (1ULL << i)) &&
                bpf_cpumask_test_cpu(i, p->cpus_ptr))
                bpf_cpumask_set_cpu(i, mask);
        }
        cpu = scx_bpf_pick_idle_cpu((const struct cpumask *)mask, 0);
        bpf_cpumask_release(mask);
        if (cpu >= 0)
            return cpu;
        /* no idle in the hint set: fall back to any allowed hint cpu */
        for (u32 i = 0; i < nr; i++) {
            if ((idle_prefer_cpus & (1ULL << i)) &&
                bpf_cpumask_test_cpu(i, p->cpus_ptr))
                return i;
        }
        return -1;
    }

    /* auto: idle cpu in the task's allowed mask */
    cpu = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);
    if (cpu >= 0)
        return cpu;

    /* no idle cpu: caller falls back to prev_cpu / current */
    return -1;
}

/* ======================= ops ======================= */

s32 BPF_STRUCT_OPS(soup_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
    BUMP_PERCPU(dbg_select_cpu_calls);
    struct task_metrics *m = get_metrics(p->pid);
    if (!m) {
        BUMP_PERCPU(dbg_metrics_miss);
        return prev_cpu;
    }

    /* A task flagged rt_stream gets a dedicated core and a local insertion */
    if (m->klass == 1 && m->in_rt) {
        /*
         * NEVER override kthread affinity. Per-cpu kthreads (scx watchdog,
         * rcu, etc) have a pinned cpu and CANNOT migrate; inserting them to
         * SCX_DSQ_LOCAL_ON|wrong_cpu strands them -> watchdog stall. They
         * are not SDR streams; use the default path for them.
         */
        if (p->flags & PF_KTHREAD) {
            bool is_idle = false;
            return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
        }

        s32 target = pick_tier0_cpu(p, prev_cpu);
        bool target_is_idle_hint = (target >= 0);
        if (target < 0)
            /* no idle cpu anywhere (full saturation): preempt the WAKER's
             * cpu. The wakeup context IS the preemption point; kicking
             * prev_cpu instead means waiting for that (busy) cpu's next
             * tick, which under a 20ms batch slice is 1-6ms of tail. The
             * waker cpu preempts instantly: it is already in the wakeup
             * path and the running task there is the one to displace. */
            target = bpf_get_smp_processor_id();
        if (target < 0)
            target = prev_cpu;

        /*
         * Only insert to a target cpu if it's allowed for this task and
         * online. Otherwise fall back to the current cpu's local DSQ to
         * guarantee forward progress (a lost task = runnable stall =
         * watchdog kill).
         */
        if (bpf_cpumask_test_cpu(target, p->cpus_ptr)) {
            /*
             * Always kick after LOCAL_ON insert. Claiming a cpu idle via
             * test_and_clear_cpu_idle without a subsequent kick can leave it
             * idle-claimed but never woken -> the cpu stops ticking the scx
             * watchdog -> "watchdog failed to check in" stall. Busy cpus
             * need the preempt kick anyway.
             *
             * Only claim idle when pick actually found an idle cpu. Under
             * full saturation pick returns -1 and we fall back to prev_cpu,
             * which is BUSY: test_and_clear on a busy cpu lies to the kernel
             * and can defeat the preempt path.
             */
            if (target_is_idle_hint)
                scx_bpf_test_and_clear_cpu_idle(target);
            scx_bpf_kick_cpu(target, SCX_KICK_PREEMPT);
            scx_bpf_dsq_insert___v2(p, SCX_DSQ_LOCAL_ON | target,
                                    rt_max_slice_ns,
                                    SCX_ENQ_CPU_SELECTED | SCX_ENQ_PREEMPT);
        } else {
            /*
             * Target not allowed for this task (affinity race / cpu
             * offline). Insert to the WAKER's local DSQ and return the
             * waker cpu so the forward-progress claim holds: LOCAL resolves
             * to the returned cpu. Do NOT return target here.
             */
            s32 cur = bpf_get_smp_processor_id();
            scx_bpf_dsq_insert___v2(p, SCX_DSQ_LOCAL, rt_max_slice_ns,
                                    SCX_ENQ_CPU_SELECTED);
            BUMP_PERCPU(rt_direct_count);
            return cur;
        }
        BUMP_PERCPU(rt_direct_count);
        return target;
    }

    /*
     * Batch / interactive tasks: fall back to the kernel's own default
     * cpu selection (scx_bpf_select_cpu_dfl). Returning raw prev_cpu here
     * mis-routes per-cpu kthreads (like the scx watchdog) to wrong cpus
     * and starves them -> "watchdog failed to check in" -> runnable stall.
     * The default path handles idle selection + kthread affinity properly.
     */
    {
        bool is_idle = false;
        return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
    }
}

void BPF_STRUCT_OPS(soup_enqueue, struct task_struct *p, u64 enq_flags)
{
    u32 tid = p->pid;
    struct task_metrics *m = get_metrics(tid);
    BUMP_PERCPU(dbg_enqueue_calls);
    if (!m)
        return;

    u64 now = now_ns();

    /*
     * Sample sleep time: time between this enqueue and the last enqueue
     * that ended in a stopping(). last_enq_ns is stamped in stopping(), so
     * the delta is how long the task slept (plus wake delay). Kept for
     * observability; not used in classification.
     */
    if (m->last_enq_ns) {
        u64 slept = now - m->last_enq_ns;
        m->sleep_ewma = ewma_update(m->sleep_ewma, slept, 4); /* alpha 1/16 */
    }

    /* Re-classify unless within promotion window */
    s32 k = classify_task(m, now);
    set_klass(m, k, now);
    if (verbose) {
        if (k == 1)
            vdbg("soup:rt", p->pid, m->vsw_ratio_ewma);
        else if (k == 3)
            vdbg("soup:batch", p->pid, m->slice_ewma);
        else
            vdbg("soup:int", p->pid, m->slice_ewma);
    }
    if (k == 1)
        BUMP_PERCPU(dbg_classify_rt);
    else if (k == 3)
        BUMP_PERCPU(dbg_classify_batch);
    else
        BUMP_PERCPU(dbg_classify_interactive);

    if (k == 1) {
        /*
         * BISECT: enqueue-side LOCAL_ON insert + kick suspected in the 30s
         * stall (counters freeze ~1s after attach). Route rt-classified
         * tasks through the plain local DSQ for now.
         */
        scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, rt_max_slice_ns, enq_flags);
        BUMP_PERCPU(rt_wake_count);
        return;
    }

    if (k == 3) {
        /*
         * batch: long slice on the local rq. We do NOT use a custom DSQ +
         * ops.dispatch: on this kernel defining ops.dispatch changes local-rq
         * auto-drain semantics and causes runnable-task stalls. Instead batch
         * tasks get a long slice on SCX_DSQ_LOCAL and are preempted by
         * rt/interactive wakeups via SCX_KICK_PREEMPT in select_cpu, which
         * yields the same tier separation (batch fills idle cycles, streams
         * preempt it instantly).
         */
        scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, batch_min_slice_ns, enq_flags);
        BUMP_PERCPU(batch_absorbed_count);
        return;
    }

    /* default interactive: local FIFO queue (kernel auto-drains SCX_DSQ_LOCAL) */
    scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, interactive_slice_ns, enq_flags);
}

/*
 * No ops.dispatch: on this kernel defining it (even empty) changes local-rq
 * auto-drain semantics and causes runnable-task stalls. All tasks go to
 * SCX_DSQ_LOCAL and the kernel drains that. Tier separation happens via
 * slice length + SCX_KICK_PREEMPT in select_cpu (rt/interactive preempt
 * batch instantly). Not registered in SCX_OPS_DEFINE below.
 */

void BPF_STRUCT_OPS(soup_running, struct task_struct *p)
{
    struct task_metrics *m = get_metrics(p->pid);
    if (!m)
        return;
    m->last_start = now_ns();
}

void BPF_STRUCT_OPS(soup_stopping, struct task_struct *p, bool is_preempt)
{
    struct task_metrics *m = get_metrics(p->pid);
    if (!m)
        return;

    u64 now = now_ns();
    u64 slice = now - m->last_start;
    if (slice > 0) {
        m->slice_ewma = ewma_update(m->slice_ewma, slice, 3); /* alpha 1/8 */
        m->runs++;
    }
    if (is_preempt)
        m->no_vol_switches++;
    else
        m->vol_switches++;

    /* voluntary switch ratio, per-mille EWMA */
    u64 total = m->vol_switches + m->no_vol_switches;
    if (total > 0) {
        u64 ratio = m->vol_switches * 1000 / total;
        m->vsw_ratio_ewma = ewma_update(m->vsw_ratio_ewma, ratio, 4);
    }

    m->last_stop = now;
    m->last_enq_ns = now; /* next enqueue measures wake latency from here */
}

s32 BPF_STRUCT_OPS(soup_init_task, struct task_struct *p, struct scx_init_task_args *args)
{
    u32 tid = p->pid;
    struct task_metrics zero = {};
    struct task_metrics *m;

    BUMP_PERCPU(dbg_init_task_calls);
    bpf_map_update_elem(&task_metrics_map, &tid, &zero, BPF_ANY);
    m = get_metrics(tid);
    if (!m)
        return 0;
    /* NOTE: no SCHED_FIFO/RR special case. RT-class tasks bypass sched_ext
     * entirely, so checking p->policy here is a no-op. in_rt is the
     * classifier's sticky flag (set_klass), not a policy marker. */
    return 0;
}

void BPF_STRUCT_OPS(soup_exit_task, struct task_struct *p, struct scx_exit_task_args *args)
{
    u32 tid = p->pid;
    bpf_map_delete_elem(&task_metrics_map, &tid);
}

void BPF_STRUCT_OPS(soup_enable, struct task_struct *p)
{
}

void BPF_STRUCT_OPS(soup_disable, struct task_struct *p)
{
}

SCX_OPS_DEFINE(soup_ops,
               .select_cpu          = (void *)soup_select_cpu,
               .enqueue             = (void *)soup_enqueue,
               .running             = (void *)soup_running,
               .stopping            = (void *)soup_stopping,
               .init_task           = (void *)soup_init_task,
               .exit_task           = (void *)soup_exit_task,
               .enable              = (void *)soup_enable,
               .disable             = (void *)soup_disable,
               .name                = "soup");