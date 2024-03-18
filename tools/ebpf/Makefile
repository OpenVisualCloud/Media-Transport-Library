# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

.PHONY: all
all: et lcore_monitor

.PHONY: clean
clean:
	rm -rf lcore_monitor et *.o *.skel.h vmlinux.h

vmlinux.h:
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

# Build BPF code, strip useless info
%.bpf.o: %.bpf.c vmlinux.h
	clang -g -O2 -target bpf -c $(filter %.c,$^) -o $@
	llvm-strip -g $@

# Generate BPF skeletons
%.skel.h: %.bpf.o
	bpftool gen skeleton $< > $@

# Get a list of all the .skel.h files
SKEL_FILES := $(patsubst %.bpf.c,%.skel.h,$(wildcard *.bpf.c))

et: et.c fentry.skel.h
	gcc -Wall -o $@ $(filter %.c,$^) -lxdp -lbpf -lelf -lz

# Build lcore_monitor user prog
lcore_monitor: lcore_monitor.c lcore_monitor.skel.h
	gcc -Wall -o $@ $(filter %.c,$^) -lbpf -lelf -lz
