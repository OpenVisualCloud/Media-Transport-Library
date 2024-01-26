# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

.PHONY: all
all: et xsk.xdp.o lcore_monitor_kern.o lcore_monitor_kern.skel.h lcore_monitor

.PHONY: clean
clean:
	rm -rf et *.o *.skel.h lcore_monitor vmlinux.h

vmlinux.h:
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

# Build BPF code, strip useless info
%.bpf.o: %.bpf.c vmlinux.h
	clang -g -O2 -target bpf -c $(filter %.c,$^) -o $@
	llvm-strip -g $@

# Build AF_XDP kernel prog
xsk.xdp.o: xsk.xdp.c
	clang -g -O2 -target bpf -c xsk.xdp.c -o $@
	llvm-strip -g $@

# Generate BPF skeletons
%.skel.h: %.bpf.o
	bpftool gen skeleton $< > $@

# Get a list of all the .skel.h files
SKEL_FILES := $(patsubst %.bpf.c,%.skel.h,$(wildcard *.bpf.c))

et: et.c $(SKEL_FILES)
	gcc -Wall -o $@ $(filter %.c,$^) -include $(SKEL_FILES) -lxdp -l:libbpf.a -lelf -lz

# Build lcore_monitor_kern ebpf prog
lcore_monitor_kern.o: lcore_monitor_kern.c lcore_monitor.h
	clang -g -O2 -target bpf -c lcore_monitor_kern.c -o $@
	llvm-strip -g $@

# Generate lcore_monitor_kern skeletons
lcore_monitor_kern.skel.h: lcore_monitor_kern.o
	bpftool gen skeleton $< > $@

# Build lcore_monitor user prog
lcore_monitor: lcore_monitor.c lcore_monitor.h
	gcc -Wall -o lcore_monitor lcore_monitor.c -lbpf -lelf