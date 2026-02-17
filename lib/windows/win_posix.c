/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "win_posix.h"

#ifdef WINDOWSENV

char szName[] = "STSharedMemory";
static uint32_t numberofmapping = 0;
static struct filemap_info map_info[1024];

int shmget(key_t key, size_t size, int shmflg) {
  MTL_MAY_UNUSED(key);
  MTL_MAY_UNUSED(shmflg);
  int id = -1;
  HANDLE hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                      size >> 32, size & 0xffffffff, szName);
  if (hMapFile != NULL) {
    map_info[numberofmapping].maphandle = hMapFile;
    map_info[numberofmapping].size = size;
    id = numberofmapping;
    numberofmapping++;
  }
  return id;
}

void* shmat(int shmid, const void* shmaddr, int shmflg) {
  MTL_MAY_UNUSED(shmaddr);
  MTL_MAY_UNUSED(shmflg);
  void* pBuf =
      MapViewOfFile(map_info[shmid].maphandle, FILE_MAP_ALL_ACCESS, 0,
                    map_info[shmid].size >> 32, map_info[shmid].size & 0xffffffff);
  return pBuf;
}

int shmdt(const void* shmaddr) {
  int ret = UnmapViewOfFile(shmaddr);
  return ret ? 0 : 1;
}

int shmctl(int shmid, int cmd, struct shmid_ds* buf) {
  MTL_MAY_UNUSED(shmid);

  if (cmd == IPC_STAT) {
    buf->shm_nattch = 1;
    return 0;
  } else
    return (-1);
}

#define POW10_7 10000000
#define POW10_9 1000000000

/* Number of 100ns-seconds between the beginning of the Windows epoch
 * (Jan. 1, 1601) and the Unix epoch (Jan. 1, 1970)
 */
#define DELTA_EPOCH_IN_100NS INT64_C(116444736000000000)

#endif
