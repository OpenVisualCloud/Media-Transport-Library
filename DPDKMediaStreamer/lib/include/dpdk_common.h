#ifndef _DPDK_COMMON_H_
#define _DPDK_COMMON_H_

#include <rte_atomic.h>
#include <rte_build_config.h>
#include <rte_common.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_hexdump.h>
#include <rte_kni.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_per_lcore.h>

#if (RTE_VER_YEAR >= 21)

#include <rte_mbuf.h>

typedef struct pktpriv_data_s
{
	uint64_t resv1[15]; /* dynamic fields are implemented after rte_mbuf, preventing overwrite*/
	uint64_t timestamp;
} pktpriv_data_t;
#endif

/* Workaround for malicious driver issue */
#define ST_NIC_DRIVER_WA (0)
/* all in next dpdk version should be testes is needed,
                   and should be removed if OK,
                   if will not be OK we should fix drver */
#if ST_NIC_DRIVER_WA
/*
        Bellow defines are used in workaround for DPSKMS-482.
        This magic numbers are from debug session when this errow was generated.
        This problem should be solved diuring audio implementanion
*/
#define ST_NIC_DRIVER_WA_PKT_LEN_17 (17)
#define ST_NIC_DRIVER_WA_PKT_LEN_9728 (9728)
#define ST_NIC_DRIVER_WA_PKT_LEN_262144 (262144)
#define ST_NIC_DRIVER_WA_NB_SEG_8 (8)
#define ST_NIC_DRIVER_WA_NB_SEG_256 (256)
#define ST_NIC_DRIVER_WA_NB_SEG_9674 (9674)
#endif	// ST_NIC_DRIVER_WA

unsigned short siblingCore(unsigned short core);

#endif /* _DPDK_COMMCON_H_ */
