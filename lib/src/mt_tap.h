/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_TAP_HEAD_H_
#define _MT_LIB_TAP_HEAD_H_

#include "mt_main.h"

#ifdef MTL_HAS_TAP
#ifdef __MINGW32__
#include <ddk/ndisguid.h>
#else
#include <ndisguid.h>
#endif

#define TAP_IOV_DEFAULT_MAX 1024

#define ST_TAP_CTL_CODE(code) \
  CTL_CODE(FILE_DEVICE_UNKNOWN, code, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define ST_IOCTL_GET_TAPMAC ST_TAP_CTL_CODE(1)
#define ST_IOCTL_GET_TAPVER ST_TAP_CTL_CODE(2)
#define ST_IOCTL_GET_TAPMTU ST_TAP_CTL_CODE(3)
#define ST_IOCTL_GET_TAPINFO ST_TAP_CTL_CODE(4)
#define ST_IOCTL_SET_TAPSTATUS ST_TAP_CTL_CODE(6)

#define USERMODEDEVICEDIR "\\\\.\\Global\\"
#define TAP_WIN_SUFFIX ".tap"
#define TAP_WIN_DRIVERNAME "TAP-Windows Adapter"

enum rte_tuntap_type {
  ETH_TUNTAP_TYPE_UNKNOWN,
  ETH_TUNTAP_TYPE_TUN,
  ETH_TUNTAP_TYPE_TAP,
  ETH_TUNTAP_TYPE_MAX,
};

enum windows_driver_type {
  WINDOWS_DRIVER_UNSPECIFIED,
  WINDOWS_DRIVER_TAP_WINDOWS6,
  WINDOWS_DRIVER_WINTUN
};

#define IOSTATE_INITIAL 0
#define IOSTATE_QUEUED 1 /* overlapped I/O has been queued */
#define IOSTATE_IMMEDIATE_RETURN \
  2 /* I/O function returned immediately without queueing */

struct overlapped_io {
  int iostate;
  OVERLAPPED overlapped;
  DWORD size;
  DWORD flags;
  int status;
};

struct iovec {
  void* iov_base; /* Pointer to data. */
  size_t iov_len; /* Length of data. */
};

struct tap_rt_context {
  struct rte_mempool* mp;
  struct rte_mbuf* pool;
  struct iovec (*iovecs)[];
  HANDLE tap_handle;
  struct overlapped_io reads;
  struct overlapped_io writes;
  char tap_name[MAX_PATH];
  uint8_t ip_addr[MTL_IP_ADDR_LEN];
  struct rte_ether_addr mac_addr;
  unsigned int lcore;
  bool has_lcore;
  bool flow_control;
};

int st_tap_init(struct mtl_main_impl* impl);
int st_tap_uinit(struct mtl_main_impl* impl);
int st_tap_handle(struct mtl_main_impl* impl, enum mtl_port port,
                  struct rte_mbuf** rx_pkts, uint16_t nb_pkts);

#else
static inline int st_tap_init(struct mtl_main_impl* impl) { return 0; }
static inline int st_tap_uinit(struct mtl_main_impl* impl) { return 0; }
static inline int st_tap_handle(struct mtl_main_impl* impl, enum mtl_port port,
                                struct rte_mbuf** rx_pkts, uint16_t nb_pkts) {
  return -EIO;
}
#endif

#endif
