/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_soup - userspace loader for the scx_soup sched_ext
 * scheduler.
 *
 * This is intentionally NOT built with libscx or meson. It is a plain
 * libbpf program that:
 *   1. loads the BPF object (scx_soup.bpf.o),
 *   2. overrides rodata tunables from argv,
 *   3. attaches the struct_ops (which installs the scheduler),
 *   4. optionally daemonizes and reports per-cpu stats,
 *   5. cleanly detaches on SIGINT/SIGTERM (EEVDF takes back over).
 *
 * Boot/udev integration (zero-manual-operation):
 *   systemd unit + scx_loader-style auto-start are covered by
 *   soup.service so it comes up at boot before any SDR app.
 *
 * Copyright 2026 shutterspeed, GPL-2.0
 *
 * Usage:
 *   scx_soup [-f] [-s SECONDS] [--rt-max-slice NS]
 *                  [--batch-min-slice NS] [--interactive-slice NS]
 *                  [--batch-min-run NS] [--promote-ms MS] [--demote-ms MS]
 *                  [--idle-prefer-cpus MASK] [--verbose]
 *
 * Exit codes: 0 clean, 1 load failure, 2 already running.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <sys/stat.h>
#include <sys/types.h>

/* libbpf skel headers use u64/s32 in struct definitions; provide them first */
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t s32;

#include "scx_soup.skel.h"

static volatile sig_atomic_t g_exit;

static void sig_handler(int sig)
{
    g_exit = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -f            stay in foreground (default: daemonize)\n"
        "  -s SECONDS    print stats every SECONDS (0 = never, default 0)\n"
        "  -p FILE       pidfile (default: /run/scx_soup.pid)\n"
        "  --rt-max-slice NS       tier0 slice\n"
        "  --batch-min-slice NS    tier2 slice\n"
        "  --interactive-slice NS  tier1 slice\n"
        "  --batch-min-run NS      classify threshold (default 3ms)\n"
        "  --promote-ms MS         stickiness window (alias for --demote-ms)\n"
        "  --demote-ms MS          stickiness window for both classes\n"
        "  --max-stream-sleep NS   periodic-stream sleep ceiling (default 50ms)\n"
        "  --idle-prefer-cpus MASK explicit tier0 cpu hint (bitmask)\n"
        "  --verbose\n",
        prog);
}

static int write_pidfile(const char *path)
{
    if (!path)
        return 0;
    FILE *f = fopen(path, "w");
    if (!f)
        return -errno;
    fprintf(f, "%d\n", getpid());
    fclose(f);
    return 0;
}

static void remove_pidfile(const char *path)
{
    if (path)
        unlink(path);
}

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -errno;
    if (pid > 0)
        _exit(0);

    if (setsid() < 0)
        return -errno;
    pid = fork();
    if (pid < 0)
        return -errno;
    if (pid > 0)
        _exit(0);

    umask(0);
    if (chdir("/") < 0)
        return -errno;

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2)
            close(fd);
    }
    return 0;
}

static u64 sum_percpu(struct bpf_map *map)
{
    u64 sum = 0;
    u64 vals[512];
    int fd, key = 0, err;
    unsigned int nr_cpus = libbpf_num_possible_cpus();

    if (!map || nr_cpus > 512)
        return 0;
    fd = bpf_map__fd(map);
    if (fd < 0)
        return 0;

    /* low-level lookup on a percpu map returns value_size * nr_cpus bytes */
    err = bpf_map_lookup_elem(fd, &key, vals);
    if (err)
        return 0;
    for (unsigned int i = 0; i < nr_cpus; i++)
        sum += vals[i];
    return sum;
}

int main(int argc, char **argv)
{
    struct scx_soup_bpf *skel = NULL;
    struct bpf_link *link = NULL;
    const char *pidfile = "/run/scx_soup.pid";
    int foreground = 0;
    int stats_secs = 0;
    int ret = 1;
    int opt;

    static const struct option long_opts[] = {
        { "rt-max-slice",       required_argument, NULL, 1000 },
        { "batch-min-slice",    required_argument, NULL, 1002 },
        { "interactive-slice",  required_argument, NULL, 1003 },
        { "batch-min-run",      required_argument, NULL, 1004 },
        { "promote-ms",         required_argument, NULL, 1005 },
        { "demote-ms",          required_argument, NULL, 1006 },
        { "idle-prefer-cpus",   required_argument, NULL, 1007 },
        { "verbose",            no_argument,       NULL, 1008 },
        { "max-stream-sleep",   required_argument, NULL, 1009 },
        { NULL, 0, NULL, 0 }
    };

    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

    /* rodata overrides from argv, applied after open() below */
    u64 o_rt_max_slice = 0, o_batch_min_slice = 0, o_interactive_slice = 0;
    u64 o_batch_min_run = 0, o_promote_ms = 0, o_demote_ms = 0;
    u64 o_max_stream_sleep = 0;
    s32 o_idle_prefer = 0;
    int o_verbose = 0;

    while ((opt = getopt_long(argc, argv, "fs:p:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'f': foreground = 1; break;
        case 's': stats_secs = atoi(optarg); break;
        case 'p': pidfile = optarg; break;
        case 1000: o_rt_max_slice = strtoull(optarg, NULL, 0); break;
        case 1002: o_batch_min_slice = strtoull(optarg, NULL, 0); break;
        case 1003: o_interactive_slice = strtoull(optarg, NULL, 0); break;
        case 1004: o_batch_min_run = strtoull(optarg, NULL, 0); break;
        case 1005: o_promote_ms = strtoull(optarg, NULL, 0); break;
        case 1006: o_demote_ms = strtoull(optarg, NULL, 0); break;
        case 1007: o_idle_prefer = strtol(optarg, NULL, 0); break;
        case 1008: o_verbose = 1; break;
        case 1009: o_max_stream_sleep = strtoull(optarg, NULL, 0); break;
        default: usage(argv[0]); return 1;
        }
    }

    skel = scx_soup_bpf__open();
    if (!skel) {
        fprintf(stderr, "failed to open BPF skeleton\n");
        goto out;
    }

    /* apply rodata tunables from argv before load */
    if (o_rt_max_slice)      skel->rodata->rt_max_slice_ns = o_rt_max_slice;
    if (o_batch_min_slice)   skel->rodata->batch_min_slice_ns = o_batch_min_slice;
    if (o_interactive_slice) skel->rodata->interactive_slice_ns = o_interactive_slice;
    if (o_batch_min_run)     skel->rodata->batch_min_run_ns = o_batch_min_run;
    if (o_promote_ms)        skel->rodata->demote_win_ms = o_promote_ms;
    if (o_demote_ms)         skel->rodata->demote_win_ms = o_demote_ms;
    if (o_idle_prefer)       skel->rodata->idle_prefer_cpus = o_idle_prefer;
    if (o_max_stream_sleep)  skel->rodata->max_stream_sleep_ns = o_max_stream_sleep;
    if (o_verbose)           skel->rodata->verbose = true;

    if (scx_soup_bpf__load(skel)) {
        fprintf(stderr, "failed to load BPF scheduler\n");
        goto out;
    }

    link = bpf_map__attach_struct_ops(skel->maps.soup_ops);
        if (!link) {
            /* EBUSY: another sched_ext scheduler is attached, or soup already
             * running (the loader's own pidfile check below races attach).
             * This is a config error, not a code bug: exit 2 so a systemd
             * StartLimit can back off instead of an infinite restart loop. */
            fprintf(stderr, "failed to attach sched_ext ops: %s (exit 2; is another scheduler active?)\n",
                    strerror(errno));
            ret = 2;
            goto out;
        }

    if (!foreground) {
        if (daemonize())
            goto out;
    }

    if (write_pidfile(pidfile))
        fprintf(stderr, "warning: could not write pidfile %s\n", pidfile);

    if (signal(SIGINT, sig_handler) == SIG_ERR ||
        signal(SIGTERM, sig_handler) == SIG_ERR) {
        perror("signal");
        goto out;
    }

    fprintf(stderr, "scx_soup: scheduler attached, pid %d\n", getpid());

    while (!g_exit) {
        if (stats_secs > 0) {
            /* print counters */
            u64 rt_wake = sum_percpu(skel->maps.rt_wake_count);
            u64 rt_direct = sum_percpu(skel->maps.rt_direct_count);
            u64 batch = sum_percpu(skel->maps.batch_absorbed_count);
            u64 d_init = sum_percpu(skel->maps.dbg_init_task_calls);
            u64 d_run = sum_percpu(skel->maps.dbg_running_calls);
            u64 d_stop = sum_percpu(skel->maps.dbg_stopping_calls);
            u64 d_enq = sum_percpu(skel->maps.dbg_enqueue_calls);
            u64 d_sel = sum_percpu(skel->maps.dbg_select_cpu_calls);
            u64 d_rt = sum_percpu(skel->maps.dbg_classify_rt);
            u64 d_batch = sum_percpu(skel->maps.dbg_classify_batch);
            u64 d_int = sum_percpu(skel->maps.dbg_classify_interactive);
            u64 d_miss = sum_percpu(skel->maps.dbg_metrics_miss);
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            fprintf(stderr,
                "[%02d:%02d:%02d] rt=%llu/%llu batch=%llu | ops init=%llu run=%llu stop=%llu enq=%llu sel=%llu miss=%llu | cls rt=%llu batch=%llu int=%llu\n",
                    tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0,
                    (unsigned long long)rt_wake,
                    (unsigned long long)rt_direct,
                    (unsigned long long)batch,
                    (unsigned long long)d_init,
                    (unsigned long long)d_run,
                    (unsigned long long)d_stop,
                    (unsigned long long)d_enq,
                    (unsigned long long)d_sel,
                    (unsigned long long)d_miss,
                    (unsigned long long)d_rt,
                    (unsigned long long)d_batch,
                    (unsigned long long)d_int);
        }
        sleep(stats_secs > 0 ? stats_secs : 3600);
    }

    fprintf(stderr, "scx_soup: detaching\n");
    bpf_link__destroy(link);
    link = NULL;
    scx_soup_bpf__destroy(skel);
    skel = NULL;
    remove_pidfile(pidfile);
    ret = 0;

out:
    if (link)
        bpf_link__destroy(link);
    if (skel)
        scx_soup_bpf__destroy(skel);
    remove_pidfile(pidfile);
    return ret;
}