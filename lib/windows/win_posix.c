/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include "win_posix.h"

#ifdef WINDOWSENV
char szName[] = "STSharedMemory";
static uint32_t numberofmapping = 0;
static struct filemap_info map_info[1024];
int shmget(key_t key, size_t size, int shmflg) {
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
  if (cmd == IPC_STAT) {
    buf->shm_nattch = 1;
    return 0;
  } else
    return (-1);
}

#endif
