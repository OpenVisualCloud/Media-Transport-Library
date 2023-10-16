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

int clock_adjtime(int clk_id, struct timex* tp) {
  if (clk_id == CLOCK_REALTIME) {
    if (tp->modes & ADJ_SETOFFSET) {
      SYSTEMTIME st;
      GetSystemTime(&st);
      FILETIME ft;
      SystemTimeToFileTime(&st, &ft);
      ULARGE_INTEGER ui;
      ui.LowPart = ft.dwLowDateTime;
      ui.HighPart = ft.dwHighDateTime;
      ui.QuadPart += tp->time.tv_sec * 10000000ULL;
      if (tp->modes & ADJ_NANO) {
        ui.QuadPart += tp->time.tv_usec / 100;
      } else {
        ui.QuadPart += tp->time.tv_usec * 10;
      }
      ft.dwLowDateTime = ui.LowPart;
      ft.dwHighDateTime = ui.HighPart;
      FileTimeToSystemTime(&ft, &st);
      if (!SetSystemTime(&st)) {
        return -1;
      }
    } else if (tp->modes == ADJ_FREQUENCY) {
      if (!SetSystemTimeAdjustmentPrecise((DWORD)(tp->freq * 10), FALSE)) {
        return -1;
      }
    } else {
      /* Not supported in Windows */
      return -1;
    }
  } else {
    /* Unknown clock ID */
    return -1;
  }

  return 0;
}

#endif
