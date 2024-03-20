# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

.PHONY: all
all: et lcore_monitor udp_monitor

.PHONY: clean
clean:
	rm -rf udp_monitor lcore_monitor et *.o *.skel.h vmlinux.h

vmlinux.h:
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

# Build et
fentry.bpf.o: fentry.bpf.c et.h vmlinux.h
	clang -g -O2 -target bpf -c fentry.bpf.c -o $@
	llvm-strip -g $@
fentry.skel.h: fentry.bpf.o
	bpftool gen skeleton $< > $@
et: et.c fentry.skel.h et.h
	gcc -Wall -o $@ $(filter %.c,$^) -lxdp -l:libbpf.a -lelf -lz

# Build lcore_monitor
lcore_monitor.bpf.o: lcore_monitor.bpf.c lcore_monitor.h vmlinux.h
	clang -g -O2 -target bpf -c lcore_monitor.bpf.c -o $@
	llvm-strip -g $@
lcore_monitor.skel.h: lcore_monitor.bpf.o
	bpftool gen skeleton $< > $@
lcore_monitor: lcore_monitor.c lcore_monitor.skel.h lcore_monitor.h
	gcc -Wall -o $@ $(filter %.c,$^) -l:libbpf.a -lelf -lz

# Build udp_monitor
udp_monitor.bpf.o: udp_monitor.bpf.c udp_monitor.h
	clang -g -O2 -target bpf -c udp_monitor.bpf.c -o $@
	llvm-strip -g $@
udp_monitor.skel.h: udp_monitor.bpf.o
	bpftool gen skeleton $< > $@
udp_monitor: udp_monitor.c udp_monitor.skel.h udp_monitor.h
	gcc -Wall -o $@ $(filter %.c,$^) -l:libbpf.a -lelf -lz
