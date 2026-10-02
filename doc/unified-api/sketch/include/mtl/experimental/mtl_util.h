/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_util.h - helpers of the unified MTL API, revision 0.2.
 *
 * Every function here is built only on public calls of mtl.h and mtl_mem.h; an
 * application could write it itself. They exist so that common code is written once:
 * the copy path for byte-stream essences, one-call sends from named slots, and the
 * ST 2110-40 user-data-word arithmetic.
 */

#ifndef MTL_EXPERIMENTAL_MTL_UTIL_H
#define MTL_EXPERIMENTAL_MTL_UTIL_H

#include "mtl_mem.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* Copy path (audio, ANC user data, fastmeta, packet chunks): acquire, copy `bytes`
   (splitting at the pool's unit capacity; audio media time advances by the samples sent),
   copy how->meta (a meta area, struct mtl_meta_hdr first) when set, and submit with the
   per-use fields of `how` (NULL = AUTO). A received unit is a valid `how`. Returns the
   bytes accepted. WT. */
MTL_API_WT int mtl_tx_write(mtl_session_h s, const void* data, size_t bytes,
                            const struct mtl_unit* MTL_NULLABLE how, int64_t timeout_ns);
/* Acquire slot `slot` and submit it with the per-use fields of `how`, or do nothing:
   -MTL_EAGAIN while the slot is still in flight. WT. */
MTL_API_WT int mtl_tx_send_slot(mtl_session_h s, uint32_t slot,
                                const struct mtl_unit* MTL_NULLABLE how, int64_t timeout_ns);
/* Bounded copies into or out of a leased unit (bindings, Python buffers). DPC. */
MTL_API_DPC int mtl_unit_copy_in(struct mtl_unit* u, uint32_t plane, uint64_t offset,
                                 const void* src, size_t len);
MTL_API_DPC int mtl_unit_copy_out(const struct mtl_unit* u, uint32_t plane,
                                  uint64_t offset, void* dst, size_t len);

/* ST 2110-40 helpers (RFC 8331): 10-bit user data words with parity, and the checksum
   word, over a byte run. AS. */
MTL_API_AS uint16_t mtl_anc_udw_get(const uint8_t* udw_run, uint32_t index);
MTL_API_AS void mtl_anc_udw_set(uint8_t* udw_run, uint32_t index, uint16_t word);
MTL_API_AS uint16_t mtl_anc_parity(uint8_t value);
MTL_API_AS uint16_t mtl_anc_checksum(const uint8_t* udw_run, uint32_t udw_count);
/* 1 if the parity bits of a 10-bit word are right. AS. */
MTL_API_AS int mtl_anc_parity_ok(uint16_t word);
/* RFC 8331 ANC data <-> packet table + user data words, for packet-unit ANC. AS. */
MTL_API_AS int mtl_anc_rfc8331_decode(const uint8_t* payload, uint32_t len,
                                      struct mtl_anc_packet* pkts, uint32_t max_pkts,
                                      uint8_t* udw_run, uint32_t udw_cap, uint32_t* n_pkts);
MTL_API_AS int mtl_anc_rfc8331_encode(const struct mtl_anc_packet* pkts, uint32_t n_pkts,
                                      const uint8_t* udw_run, uint8_t* payload, uint32_t cap,
                                      uint32_t* len);

#if defined(MTL_LATER)
/* Kubernetes: port specs from a pod's network-status annotation (Multus, mounted with the
   downward API at `path`) for the networks listed in `networks` ("ns/name,..."; NULL =
   every network with a PCI device), in that order: name = the PCI address, sip from its
   first IP, prefix_len from `prefix_len` (the annotation has none; 0 = 24). The count
   written; -MTL_EAGAIN (PORT_ENV_UNSET) while the file or a network's entry is not there
   yet (Multus writes it, the kubelet refreshes it): retry; -MTL_EINVAL naming a malformed
   network. File I/O and JSON: a later helper, not built on MTL calls. CP. */
MTL_API_CP int mtl_port_specs_from_network_status(const char* path,
                                                  const char* MTL_NULLABLE networks,
                                                  uint8_t prefix_len,
                                                  struct mtl_port_spec* specs, uint32_t max);

/* Advanced, later phases (M12 decides): run one scheduler's tasklets on the caller's
   thread for budget_ns (manual progress), and named, versioned backend extension tables
   for knobs no option can express (the queue-meta route). */
MTL_API_CP int mtl_sched_run_once(mtl_instance_h mt, uint32_t sched, int64_t budget_ns);
MTL_API_CP int mtl_open_ext(mtl_instance_h mt, const char* name, uint32_t version,
                            void* ops, size_t ops_size);
#endif

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_UTIL_H */
