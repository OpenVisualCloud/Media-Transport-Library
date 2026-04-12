// SPDX-License-Identifier: GPL-2.0 OR MIT
//
// irq_softirq.bpf.c — CO-RE eBPF program for tracking IRQ and softirq time per CPU.
//
// Attaches to irq:irq_handler_entry/exit and softirq:softirq_entry/exit tracepoints.
//
// Compile with:
//   clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
//     -I/usr/include/bpf -c irq_softirq.bpf.c -o irq_softirq.bpf.o

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

// ─── IRQ tracking ───────────────────────────────────────────────────────────

struct irq_key {
    __u32 cpu;
    __u32 irq;
};

struct irq_stat {
    __u64 time_ns;
    __u32 count;
};

// Per-CPU: timestamp of irq_handler_entry
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} irq_start SEC(".maps");

// Per-CPU: which IRQ is currently executing
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} irq_current SEC(".maps");

// Hash: {cpu, irq} -> {time_ns, count}
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 8192);
    __type(key, struct irq_key);
    __type(value, struct irq_stat);
} irq_stats SEC(".maps");

SEC("tracepoint/irq/irq_handler_entry")
int handle_irq_entry(struct trace_event_raw_irq_handler_entry *ctx)
{
    __u32 zero = 0;
    __u64 ts = bpf_ktime_get_ns();
    __u32 irq_num = ctx->irq;

    bpf_map_update_elem(&irq_start, &zero, &ts, BPF_ANY);
    bpf_map_update_elem(&irq_current, &zero, &irq_num, BPF_ANY);

    return 0;
}

SEC("tracepoint/irq/irq_handler_exit")
int handle_irq_exit(struct trace_event_raw_irq_handler_exit *ctx)
{
    __u32 zero = 0;
    __u64 *start_ts = bpf_map_lookup_elem(&irq_start, &zero);
    __u32 *irq_num = bpf_map_lookup_elem(&irq_current, &zero);

    if (!start_ts || !irq_num || *start_ts == 0)
        return 0;

    __u64 delta = bpf_ktime_get_ns() - *start_ts;
    __u32 cpu = bpf_get_smp_processor_id();

    struct irq_key key = { .cpu = cpu, .irq = *irq_num };
    struct irq_stat *stat = bpf_map_lookup_elem(&irq_stats, &key);
    if (stat) {
        __sync_fetch_and_add(&stat->time_ns, delta);
        __sync_fetch_and_add(&stat->count, 1);
    } else {
        struct irq_stat new_stat = { .time_ns = delta, .count = 1 };
        bpf_map_update_elem(&irq_stats, &key, &new_stat, BPF_ANY);
    }

    // Clear start
    __u64 zero_val = 0;
    bpf_map_update_elem(&irq_start, &zero, &zero_val, BPF_ANY);

    return 0;
}

// ─── Softirq tracking ──────────────────────────────────────────────────────

struct softirq_key {
    __u32 cpu;
    __u32 vec;
};

struct softirq_stat {
    __u64 time_ns;
    __u32 count;
};

// Per-CPU: timestamp of softirq_entry
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} softirq_start SEC(".maps");

// Per-CPU: which softirq vec is currently executing
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} softirq_current SEC(".maps");

// Hash: {cpu, vec} -> {time_ns, count}
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, struct softirq_key);
    __type(value, struct softirq_stat);
} softirq_stats SEC(".maps");

SEC("tracepoint/irq/softirq_entry")
int handle_softirq_entry(struct trace_event_raw_softirq *ctx)
{
    __u32 zero = 0;
    __u64 ts = bpf_ktime_get_ns();
    __u32 vec = ctx->vec;

    bpf_map_update_elem(&softirq_start, &zero, &ts, BPF_ANY);
    bpf_map_update_elem(&softirq_current, &zero, &vec, BPF_ANY);

    return 0;
}

SEC("tracepoint/irq/softirq_exit")
int handle_softirq_exit(struct trace_event_raw_softirq *ctx)
{
    __u32 zero = 0;
    __u64 *start_ts = bpf_map_lookup_elem(&softirq_start, &zero);
    __u32 *vec_num = bpf_map_lookup_elem(&softirq_current, &zero);

    if (!start_ts || !vec_num || *start_ts == 0)
        return 0;

    __u64 delta = bpf_ktime_get_ns() - *start_ts;
    __u32 cpu = bpf_get_smp_processor_id();

    struct softirq_key key = { .cpu = cpu, .vec = *vec_num };
    struct softirq_stat *stat = bpf_map_lookup_elem(&softirq_stats, &key);
    if (stat) {
        __sync_fetch_and_add(&stat->time_ns, delta);
        __sync_fetch_and_add(&stat->count, 1);
    } else {
        struct softirq_stat new_stat = { .time_ns = delta, .count = 1 };
        bpf_map_update_elem(&softirq_stats, &key, &new_stat, BPF_ANY);
    }

    // Clear start
    __u64 zero_val = 0;
    bpf_map_update_elem(&softirq_start, &zero, &zero_val, BPF_ANY);

    return 0;
}

char LICENSE[] SEC("license") = "Dual MIT/GPL";
