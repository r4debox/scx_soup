/*
 * scx_minimal.bpf.c - minimal sched_ext scheduler to isolate the stall.
 * No profiler, no classify, no rt path. enqueue -> SCX_DSQ_LOCAL,
 * select_cpu -> default. If THIS stalls, the bug is in my ops/loader/ABI;
 * if not, bisect the profiler back in.
 */
#include "common.bpf.h"
#include "user_exit_info.bpf.h"

/*
 * Same enum hardcode as scx_soup.bpf.c: the vendored enums.autogen
 * rodata vars default to 0 without a libscx-style loader populating them.
 */
#undef SCX_DSQ_LOCAL
#undef SCX_DSQ_GLOBAL
#undef SCX_ENQ_WAKEUP
#undef SCX_ENQ_CPU_SELECTED
#define SCX_DSQ_LOCAL   0x8000000000000002ULL
#define SCX_DSQ_GLOBAL  0x8000000000000001ULL
#define SCX_ENQ_WAKEUP         1ULL
#define SCX_ENQ_CPU_SELECTED   0x100000ULL

char LICENSE[] SEC("license") = "GPL";

const volatile u64 slice_ns = 8000000;

static __always_inline u64 now_ns(void)
{
    return bpf_ktime_get_ns();
}

void BPF_STRUCT_OPS(scx_minimal_enqueue, struct task_struct *p, u64 enq_flags)
{
    scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice_ns, enq_flags);
}

s32 BPF_STRUCT_OPS(scx_minimal_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
    bool is_idle = false;
    return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
}

SCX_OPS_DEFINE(scx_minimal_ops,
               .select_cpu = (void *)scx_minimal_select_cpu,
               .enqueue    = (void *)scx_minimal_enqueue,
               .name       = "minimal");