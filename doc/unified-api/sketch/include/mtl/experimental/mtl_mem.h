/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_mem.h - application memory for the unified MTL API, experimental revision 0.2.
 *
 * Library pools need nothing from this header. Include it to send from or receive into
 * memory the application owns (a framework pool, an MXL ring, a GPU-pinned host buffer),
 * to lend one session's pool to another (zero-copy forwarding), or to address a slot by
 * index. Buffer requirements come from mtl_session_query() (mtl.h). A device the
 * application drives itself gets its memory from the application's own regions.
 *
 * Rules (contract.md §9):
 * MEM1 A region is memory MTL may DMA: page aligned (hugepage aligned for hugetlbfs),
 *     refcounted, mapped into every port and DMA engine that a session using it needs.
 *     mtl_mem_close() retires it once no session, slot, in-flight unit or DMA references it.
 * MEM2 A slot is one buffer of one session's pool, named by its index. Its layout is fixed
 *     when it is attached. Any stride >= row_bytes is direct (no copy).
 * MEM3 A session whose slots are application memory always produces results: a slot is
 *     never reused before the application has read the result that frees it.
 */

#ifndef MTL_EXPERIMENTAL_MTL_MEM_H
#define MTL_EXPERIMENTAL_MTL_MEM_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- Regions ----------------------------------------------------------------------- */

#define MTL_MEM_READ 0x1u       /* TX reads it; 0 = read and write */
#define MTL_MEM_WRITE 0x2u      /* RX writes it */
#define MTL_MEM_MAP_ALL 0x4u    /* map into every port now, or fail */
#define MTL_MEM_NUMA_CHECK 0x8u /* fail unless the pages are on `numa` */
/* Device memory (GPU VRAM) by `device`: a dma-buf fd or a device address; planes without a
   CPU mapping have addr NULL (legacy gpu_direct framebuffers). MS6; until then
   -MTL_ENOTSUP (NOT_IMPLEMENTED). */
#define MTL_MEM_DEVICE 0x10u

struct mtl_mem_desc {
  uint32_t struct_size;
  uint32_t numa;           /* alloc: placement; import: with NUMA_CHECK. MTL_INDEX(node),
                              0 = the ports' */
  MTL_ADDR(void) va;       /* import: start; NULL = allocate library hugepages */
  uint64_t length;
  uint64_t flags;          /* MTL_MEM_* */
  uint64_t device;         /* MTL_MEM_DEVICE: the dma-buf fd or device address */
  uint64_t reserved[3];
};
/* A region: d->va set imports [va, va + length); d->va NULL allocates library hugepages.
   *va (may be NULL) receives the start either way. CP. (MS2) */
MTL_API_CP(2) int mtl_mem_open(mtl_instance_h mt, const struct mtl_mem_desc* d,
                               mtl_region_h* out, MTL_ADDR(void)* MTL_NULLABLE va);
static inline int mtl_mem_import(mtl_instance_h mt, const struct mtl_mem_desc* d,
                                 mtl_region_h* out) {
  return mtl_mem_open(mt, d, out, NULL);
}
static inline int mtl_mem_alloc(mtl_instance_h mt, const struct mtl_mem_desc* d,
                                mtl_region_h* out, MTL_ADDR(void)* va) {
  return mtl_mem_open(mt, d, out, va); /* d->va NULL */
}
/* Drops the caller's reference. 0: retired, the application may unmap the memory.
   MTL_RETIRING: still referenced (MEM1), keep it mapped: a device may still read or write
   it; call it again to poll; MTL_EVENT_REGION_RELEASED on the instance reports the end. 0
   for a null handle. The result must be read (MTL_MUST_CHECK). CP. */
MTL_MUST_CHECK static inline int mtl_mem_close(mtl_region_h r) {
  return mtl_close(mtl_obj(MTL_OBJ_REGION, 0, r.id), 0);
}
enum mtl_backing {
  MTL_BACKING_HUGEPAGE = 1, /* library hugepages */
  MTL_BACKING_HUGETLBFS = 2,
  MTL_BACKING_ANON = 3,  /* anonymous or pinned host memory */
  MTL_BACKING_SHMEM = 4, /* tmpfs, memfd */
  MTL_BACKING_FILE = 5,  /* page cache: copy only */
};
/* Output; the size argument versions it. */
struct mtl_mem_info {
  uint32_t backing; /* enum mtl_backing */
  uint32_t numa;    /* MTL_INDEX(node); 0 = mixed */
  MTL_ADDR(void) va; /* start */
  uint64_t length;
  uint64_t page_size;
  uint64_t mapped_ports; /* bit per port */
  uint32_t direct;       /* 0 = copy only */
  uint32_t refs;         /* sessions, slots and in-flight units */
};
/* What a region is. CP. (MS2) */
MTL_API_CP(2) int mtl_mem_get_info(mtl_region_h r, struct mtl_mem_info* info, size_t size);

/* ---- Attached pools (session flag MTL_SESSION_POOL_ATTACHED) ---------------------- */

#define MTL_ATTACH_META_IN_SLOT 0x100u /* RX meta written into the slot at meta_offset (MXL) */

/* `count` slots in the session's natural layout: slot i at offset + i * pitch, or at
   slot_offset[i]. With a null region, [va, va + length) is imported for this session and
   released when it retires. */
struct mtl_attach {
  uint32_t struct_size;
  uint32_t count;
  mtl_region_h region;
  MTL_ADDR(void) va;
  uint64_t length;
  uint64_t offset;                         /* slot 0, plane 0 */
  uint64_t pitch;                          /* 0 = unit_bytes */
  const uint64_t* MTL_NULLABLE slot_offset; /* count entries: interlaced fields, scattered */
  uint64_t plane_offset[MTL_MAX_PLANES];    /* from the slot start; 0 = after the previous */
  uint32_t stride[MTL_MAX_PLANES];          /* 0 = natural; one per pool */
  uint64_t flags;                           /* MTL_ATTACH_* and MTL_MEM_* (access, mapping) */
  uint64_t meta_offset;
  uint32_t meta_capacity;
  uint32_t reserved0;
  uint64_t cookie; /* mtl_rx_provide and mtl_tx_acquire_layout: names the buffer in the unit
                      (RX) and in the result (TX); mtl_session_attach: must be 0 */
  uint64_t reserved[1];
};
/* CREATED or STOPPED; may be called again to append slots. Validates span, stride,
   alignment, access and the region budget, with the reason of the failure. With
   pool_count set, start is -MTL_EINVAL (POOL_TOO_SMALL) until that many slots are
   attached; with pool_count 0 the attached slots are the pool; a provide session
   (mtl_rx_provide) needs no slot. a NULL detaches every slot: CREATED or STOPPED, with no
   lease, hold or session attached over this pool; on a provide session it hands back
   every destination (ready units are dropped) and ends provide mode. a->cookie must be 0.
   CP. (MS2) */
MTL_API_CP(2) int mtl_session_attach(mtl_session_h s, const struct mtl_attach* MTL_NULLABLE a);
/* The static part of slot `slot` (planes, meta); lease null. DP. (MS1) */
MTL_API_DP(1) int mtl_session_get_slot(mtl_session_h s, uint32_t slot, struct mtl_unit* u);
/* A library pool as a region, so another session can attach over it (forwarding). The
   region holds a reference: mtl_mem_close() drops it. A session attached over another's
   pool submits slot j only with hold = a lease of the owner's slot j (-MTL_EINVAL,
   HOLD_REQUIRED); the owner cannot change or detach its pool while attached sessions
   exist (-MTL_EBUSY), and its close returns 1 until they close. CP. (MS2) */
MTL_API_CP(2) int mtl_session_get_pool_region(mtl_session_h s, mtl_region_h* out);

/* ---- Requirements (output of mtl_session_query) ------------------------------------- */

struct mtl_plane_requirements {
  uint64_t min_span;     /* bytes at the natural stride */
  uint32_t row_bytes;    /* bytes per row the session reads or writes */
  uint32_t rows;         /* per unit (a field: height / 2) */
  uint32_t stride;       /* natural stride */
  uint32_t stride_align; /* 1 = byte granular */
  uint32_t offset_align;
  uint32_t reserved;
};
/* Output; the size argument of mtl_session_query versions it. */
struct mtl_buffer_requirements {
  uint32_t plane_count;
  uint32_t min_count;
  uint32_t min_count_direct; /* video: slots needed to stay direct */
  uint32_t max_count;
  struct mtl_plane_requirements plane[MTL_MAX_PLANES];
  uint32_t meta_capacity;
  uint32_t direct_possible;
  uint64_t unit_bytes;
  uint64_t internal_bytes; /* hugepage bytes MTL allocates for the session besides the
                              pool: internal frames, mempools, the slot table, stats blocks,
                              ANC wire areas (contract.md §9.4) */
  int64_t completion_latency_ns;
};

/* ---- Named slots -------------------------------------------------------------------- */

/* TX: that slot, e.g. framework surface i; -MTL_EAGAIN while it is not free (R2). WT
   (waiting: mtl.h). (MS2) */
MTL_API_WT(2) int mtl_tx_acquire_slot(mtl_session_h s, uint32_t slot, struct mtl_unit* u,
                                      int64_t timeout_ns);

/* A slot bound to a new layout per acquire: a framework buffer per frame (GStreamer TX,
   moving-cursor producers); u->cookie starts as one->cookie (a template applied later
   overwrites it: set how.cookie = one->cookie). WT (waiting: mtl.h). (MS2) */
MTL_API_WT(2) int mtl_tx_acquire_layout(mtl_session_h s, const struct mtl_attach* one,
                                        struct mtl_unit* u, int64_t timeout_ns);
/* RX destination for one unit (the legacy query_ext_frame), for producers that choose it
   per unit: a cursor through an arena, a grain claimed at arrival. Frameworks wrap
   library slots instead (contract §9.8). one: count 1, a layout inside a region already
   mapped into every device of the session (else -MTL_EINVAL, SPAN), cookie != 0 (else
   -MTL_EINVAL, FIELD_REQUIRED). An RX video session created with MTL_SESSION_POOL_ATTACHED,
   not RX_BY_INDEX, with no slot attached (-MTL_EINVAL); the first success makes it a
   provide session (start then needs no slot) until a detach. At most pool_count held:
   provided and not yet the caller's again (one more: -MTL_ENOSPC, PROVIDE_FULL). CREATED,
   ARMED, STOPPED, RUNNING; else -MTL_ESHUTDOWN or the error code, and nothing is taken.
   One providing thread at a time. Returns g >= 1, the generation of the destination
   (status.provide_gen at its acceptance). Every stop (a no-op stop included), discard,
   attach(s, NULL) and close is one hand-back: it moves the internal tag to G + 1, returns
   every destination queued with a generation older than G + 1 (discard, detach and close
   also those of the ready units they drop), then publishes provide_gen = G + 1 before it
   returns; a destination accepted meanwhile is re-tagged G + 1 and stays held, or is
   withdrawn and the call is -MTL_ESHUTDOWN (contract §9.11, provide_model.py). A unit takes the oldest
   destination held when its first packet lands; units are delivered in reception order
   with u.cookie the destination's and u.slot UINT32_MAX; a unit dropped before delivery
   gives its destination back, taken first next. A unit that finds none is missed
   (rx.units_missed_pool_full, the next unit's missed_before), never placed in library
   memory; with MTL_SESSION_RX_LATEST it takes the destination of the oldest unread unit.
   Never zero-filled, no tail. A delivered destination is the caller's again once its
   lease is released and every TX unit holding it has its result. After a hand-back, every
   destination of generation < status.provide_gen not dequeued is the caller's again,
   except after stop those of ready units, which dequeue still returns. DP. (MS2) */
MTL_API_DP(2) int mtl_rx_provide(mtl_session_h s, const struct mtl_attach* one);

MTL_SIZE_CHECK(mtl_mem_desc, 64);
MTL_SIZE_CHECK(mtl_mem_info, 48);
MTL_SIZE_CHECK(mtl_attach, 144);
MTL_SIZE_CHECK(mtl_plane_requirements, 32);
MTL_SIZE_CHECK(mtl_buffer_requirements, 176);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_MEM_H */
