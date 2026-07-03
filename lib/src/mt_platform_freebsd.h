/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_PLATFORM_FREEBSD_H_
#define _MT_PLATFORM_FREEBSD_H_

#ifdef __FreeBSD__

#include <arpa/inet.h>    /* For inet_addr, htonl, etc. */
#include <ifaddrs.h>      /* For getifaddrs */
#include <net/ethernet.h> /* struct ether_header, replaces net/if_arp.h */
#include <net/if.h>
#include <net/if_dl.h> /* For sockaddr_dl, LLADDR */
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/endian.h> /* FreeBSD's endian.h */
#include <sys/ioctl.h>
#include <sys/shm.h> /* For shmget/shmat/shmctl/shmdt */
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

/* FreeBSD uses CLOCK_MONOTONIC_FAST for low-overhead monotonic time */
#ifdef CLOCK_MONOTONIC_FAST
#define MT_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC_FAST
#else
#define MT_CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
#endif

/* FreeBSD's pthread_setaffinity_np uses cpuset_t */
#include <sys/cpuset.h>
#include <sys/param.h>

/*
 * Compatibility: Map Linux cpu_set_t to FreeBSD cpuset_t
 * FreeBSD uses cpuset_t while Linux uses cpu_set_t, but both provide
 * CPU_ZERO, CPU_SET, CPU_CLR, CPU_ISSET macros with the same semantics.
 */
#ifndef cpu_set_t
typedef cpuset_t cpu_set_t;
#endif

/* FreeBSD uses pthread_cond_timedwait with CLOCK_REALTIME by default */
#define MT_THREAD_TIMEDWAIT_CLOCK_ID CLOCK_REALTIME

/* Temp file paths */
#define MT_FLOCK_PATH "/tmp/kahawai_lcore.lock"

/* Enable process-shared mutexes/condvars */
#define MT_ENABLE_P_SHARED

/* SIMD support - FreeBSD has same x86 intrinsics as Linux */
#ifdef MTL_HAS_AVX512
#include <immintrin.h>
#endif

/* NUMA support - declare stubs if libnuma not available */
#ifndef MTL_HAS_NUMA
/*
 * NUMA stub declarations for FreeBSD when libnuma is not available.
 * Covers all libnuma symbols used by the MTL codebase so that FreeBSD
 * builds succeed even without a system libnuma installation.
 */

/* Minimal bitmask type mirroring the real libnuma struct */
struct bitmask {
  unsigned long size;
  unsigned long* maskp;
};

/* Basic NUMA query functions */
int numa_available(void);
int numa_max_node(void);
int numa_node_of_cpu(int cpu);

/* NUMA memory allocation */
void* numa_alloc_onnode(size_t size, int node);
void numa_free(void* mem, size_t size);

/* Bitmask operations used by mt_main.c */
struct bitmask* numa_bitmask_alloc(unsigned int n);
struct bitmask* numa_bitmask_setbit(struct bitmask* bmp, unsigned int n);
void numa_bind(struct bitmask* bmp);
void numa_bitmask_free(struct bitmask* bmp);
#else
/* Use system libnuma */
#include <numa.h>
#endif

#endif /* __FreeBSD__ */
#endif /* _MT_PLATFORM_FREEBSD_H_ */
