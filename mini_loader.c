#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <bpf/libbpf.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t s32;
#include "minimal.skel.h"

static volatile sig_atomic_t g_exit;
static void on_sig(int s) { g_exit = 1; }

int main(int argc, char **argv)
{
    struct minimal_bpf *skel = minimal_bpf__open_and_load();
    struct bpf_link *link;
    if (!skel) { fprintf(stderr, "load failed\n"); return 1; }
    link = bpf_map__attach_struct_ops(skel->maps.scx_minimal_ops);
    if (!link) { fprintf(stderr, "attach failed: %s\n", strerror(errno)); return 1; }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    fprintf(stderr, "minimal attached, pid %d\n", getpid());
    while (!g_exit) sleep(1);
    bpf_link__destroy(link);
    minimal_bpf__destroy(skel);
    return 0;
}
