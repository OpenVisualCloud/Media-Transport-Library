/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef __WIN_POSIX_H__
#define __WIN_POSIX_H__

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#ifdef interface
#undef interface
#endif
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC

#define ETH_ALEN 6

#ifndef ETHERTYPE_IP
#define ETHERTYPE_IP 0x0800 /* IP protocol */
#endif

#pragma pack(push)
#pragma pack(1)
struct ip {
  u_char ip_hl : 4, /* header length */
      ip_v : 4;     /* version */

  u_char ip_tos;                 /* type of service */
  short ip_len;                  /* total length */
  u_short ip_id;                 /* identification */
  short ip_off;                  /* fragment offset field */
  u_char ip_ttl;                 /* time to live */
  u_char ip_p;                   /* protocol */
  u_short ip_sum;                /* checksum */
  struct in_addr ip_src, ip_dst; /* source and dest address */
};

struct ether_header {
  uint8_t ether_dhost[ETH_ALEN];
  uint8_t ether_shost[ETH_ALEN];
  uint16_t ether_type;
} __packed;

struct udphdr {
  u_short uh_sport; /* source port */
  u_short uh_dport; /* destination port */
  short uh_ulen;    /* udp length */
  u_short uh_sum;   /* udp checksum */
};
#pragma pack(pop)

typedef int64_t key_t;
typedef unsigned short uid_t;
typedef unsigned short gid_t;

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

typedef struct timespec timestruc_t;

struct ipc_perm {
  key_t key;
  uid_t uid;
  gid_t gid;
  uid_t cuid;
  gid_t cgid;
  mode_t mode;

  unsigned short seq;
};

#define IPC_CREAT 0x0200
#define IPC_RMID 0x1000
#define IPC_SET 0x1001
#define IPC_STAT 0x1002

typedef uint32_t shmatt_t;

struct filemap_info {
  HANDLE maphandle;
  size_t size;
};

struct shmid_ds {
  struct ipc_perm shm_perm;
  int shm_segsz;
  timestruc_t shm_atime;
  timestruc_t shm_dtime;
  timestruc_t shm_ctime;
  pid_t shm_cpid;
  pid_t shm_lpid;
  unsigned short shm_nattch;
  unsigned short shm_unused;
  void* shm_unused2;
  void* shm_unused3;
};

int inline flock(int fd, int operation) { return 0; }

key_t inline ftok(const char* path, int id) { return 0; }

void* shmat(int shmid, const void* shmaddr, int shmflg);
int shmctl(int shmid, int cmd, struct shmid_ds* buf);
int shmdt(const void* shmaddr);
int shmget(key_t key, size_t size, int shmflg);

typedef void* pthread_cond_t;
typedef int pthread_condattr_t;
#ifdef __ST_BUILD__  // only lib need this typedef, app do not need
typedef rte_cpuset_t cpu_set_t;
#endif
#define localtime_r(T, Tm) (localtime_s(Tm, T) ? NULL : Tm)
int pthread_cond_signal(pthread_cond_t* cv);
int pthread_cond_init(pthread_cond_t* cv, const pthread_condattr_t* a);
int pthread_cond_wait(pthread_cond_t* cv, pthread_mutex_t* external_mutex);
int pthread_cond_destroy(pthread_cond_t* cv);
#endif
