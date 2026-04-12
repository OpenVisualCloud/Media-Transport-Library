// SPDX-License-Identifier: GPL-2.0 OR MIT
//
// sched_switch.bpf.c — CO-RE eBPF program for tracking per-CPU, per-task runtime.
//
// Attaches to sched:sched_switch tracepoint and maintains a BPF hash map
// keyed by {cpu, pid, tid} with values {runtime_ns, switch_count, comm}.
//
// Compile with:
//   clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
//     -I/usr/include/bpf -c sched_switch.bpf.c -o sched_switch.bpf.o

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

// ─── Map key and value ──────────────────────────────────────────────────────

struct task_key {
    __u32 cpu;
    __u32 pid;   // tgid (thread group leader)
    __u32 tid;   // actual task id
};

struct task_runtime {
    __u64 runtime_ns;
    __u32 switches;
    char  comm[16];
};

// Per-CPU array to track when the current task started running
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);  // timestamp when prev task started
} start_ts SEC(".maps");

// Hash map: {cpu, pid, tid} -> {runtime_ns, switches, comm}
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, struct task_key);
    __type(value, struct task_runtime);
} task_runtime SEC(".maps");

// Per-CPU array to track the currently running task's key
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct task_key);
} current_task SEC(".maps");

// ─── sched_switch tracepoint ────────────────────────────────────────────────

// tp/sched/sched_switch args: prev_comm, prev_pid, prev_prio, prev_state,
//                              next_comm, next_pid, next_prio
SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
    __u32 cpu = bpf_get_smp_processor_id();
    __u64 now = bpf_ktime_get_ns();
    __u32 zero = 0;

    // ── Account runtime for the previous (outgoing) task ────────────

    __u64 *start = bpf_map_lookup_elem(&start_ts, &zero);
    if (start && *start > 0) {
        struct task_key *prev_key = bpf_map_lookup_elem(&current_task, &zero);
        if (prev_key) {
            __u64 delta = now - *start;
            struct task_runtime *rt = bpf_map_lookup_elem(&task_runtime, prev_key);
            if (rt) {
                __sync_fetch_and_add(&rt->runtime_ns, delta);
                __sync_fetch_and_add(&rt->switches, 1);
            } else {
                struct task_runtime new_rt = {};
                new_rt.runtime_ns = delta;
                new_rt.switches = 1;
                bpf_probe_read_kernel_str(new_rt.comm, sizeof(new_rt.comm),
                                          ctx->prev_comm);
                bpf_map_update_elem(&task_runtime, prev_key, &new_rt, BPF_ANY);
            }
        }
    }

    // ── Set up tracking for the next (incoming) task ────────────────

    // Record the start timestamp
    bpf_map_update_elem(&start_ts, &zero, &now, BPF_ANY);

    // Record the next task's key
    struct task_key next_key = {};
    next_key.cpu = cpu;
    // BPF_CORE_READ from the tracepoint context
    next_key.tid = ctx->next_pid;  // In kernel, pid == tid
    next_key.pid = next_key.tid;   // We'll use tgid from task_struct if needed

    // Try to get tgid from current task
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    if (task) {
        next_key.pid = BPF_CORE_READ(task, tgid);
    }

    bpf_map_update_elem(&current_task, &zero, &next_key, BPF_ANY);

    // Pre-create entry for the next task if not exists
    struct task_runtime *existing = bpf_map_lookup_elem(&task_runtime, &next_key);
    if (!existing) {
        struct task_runtime new_rt = {};
        bpf_probe_read_kernel_str(new_rt.comm, sizeof(new_rt.comm),
                                  ctx->next_comm);
        bpf_map_update_elem(&task_runtime, &next_key, &new_rt, BPF_NOEXIST);
    }

    return 0;
}

char LICENSE[] SEC("license") = "Dual MIT/GPL";
