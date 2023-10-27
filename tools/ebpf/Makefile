# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

.PHONY: all
all: et

.PHONY: clean
clean:
	rm -rf et *.o *.skel.h

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

et: et.c $(SKEL_FILES)
	gcc -Wall -o $@ $(filter %.c,$^) -include $(SKEL_FILES) -l:libbpf.a -lelf -lz
