/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_packet.h - packet units (RTP passthrough) for the unified MTL API, revision 0.2.
 *
 * With unit = MTL_UNIT_PACKETS a session moves app-built RTP packets instead of frames,
 * on any essence or on the generic RTP essence (ST 2022-6, custom payloads). The verbs are
 * the frame verbs; only the unit's shape differs:
 *
 *   TX  mtl_tx_acquire lends a chunk of `rows` packet slots of row_bytes capacity, and the
 *       meta area is the packet table, one struct mtl_pkt_tx per slot (no meta header);
 *       each entry's `data` is its slot, so the slots may be separate buffers (the data
 *       rooms of NIC buffers) or one region; plane 0 addresses them only when they are
 *       contiguous (addr NULL otherwise). Write RTP header + payload at data, set its
 *       length, set unit.used to the packets used, submit; the table is validated and
 *       copied at submit. The chunk that ends a frame (or field) carries
 *       MTL_SUBMIT_UNIT_END. One result per chunk.
 *   RX  mtl_rx_dequeue lends a chunk of 1..N received packets: the meta area is the table,
 *       one struct mtl_pkt_rx per packet (address, length, leg, sequence, gap, arrival
 *       time); unit.used is the count. Release it when done. By default the packets are
 *       copied into the chunk in the caller, so NIC buffers are held only briefly;
 *       MTL_PKT_RX_LEND points the table into NIC buffers instead (zero copy), bounded by
 *       packet.rx_ring_packets.
 *
 * The library writes Ethernet, IP and UDP, and of the RTP header only the fields named in
 * packet.set_fields (0 = verbatim, every byte from the RTP header on is the app's). It
 * paces by packet.pacing, duplicates every packet on both ST 2022-7 legs (identical RTP),
 * and on RX removes duplicates by sequence number and RTP timestamp, within
 * rx.skew_budget_ns of packets (ST 2022-7 Annex A). No DPDK buffer is ever exposed and no
 * application code runs on a tasklet.
 */

#ifndef MTL_EXPERIMENTAL_MTL_PACKET_H
#define MTL_EXPERIMENTAL_MTL_PACKET_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* packet.set_fields: RTP header fields the library writes on TX */
#define MTL_PKT_SET_TIMESTAMP 0x1u /* floor(M * rate) from the unit's media time, one per
                                     unit (audio: per packet); -MTL_EINVAL for SMPTE2022-6,
                                     whose packets each carry their own (mtl.h) */
#define MTL_PKT_SET_SEQ 0x2u       /* 16-bit, plus the extended sequence of RFC 4175 (video)
                                     or RFC 8331 (ANC), the payload's first two bytes */
#define MTL_PKT_SET_SSRC_PT 0x4u   /* from sc.ssrc and sc.payload_type */
#define MTL_PKT_SET_MARKER 0x8u    /* the essence's marker rule: M on the last packet of a
                                     unit (video: of a frame or field; ANC: also the empty
                                     packet), M = 0 on every audio and fastmeta packet
                                     (ST 2110-31 §5.3, ST 2110-41 §5.2) */

/* packet.packet_flags */
#define MTL_PKT_TX_VALIDATE 0x1u     /* check version, PT, timestamps, marker (by the
                                        MTL_PKT_SET_MARKER rule): count, never fix */
#define MTL_PKT_RX_INCLUDE_L2 0x2u   /* RX data starts at the Ethernet header */
#define MTL_PKT_RX_NO_DEDUP 0x4u     /* deliver both legs' copies (analysers) */
#define MTL_PKT_RX_UNIT_ALIGNED 0x8u /* a chunk never spans two units */
#define MTL_PKT_SPLIT 0x10u          /* plane 0 header slots, plane 1 payload slots */
#define MTL_PKT_RX_LEND 0x20u        /* RX: no copy; dequeue is then DP instead of DPC */

/* Limits: an RTP packet in a slot is at most sc.max_udp_payload (1452: the Standard
   UDP Size Limit minus the UDP header, ST 2110-10 §6.3), never above the port MTU minus IP
   and UDP headers. */
#define MTL_UDP_HDR_BYTES 8
#define MTL_IPV4_HDR_BYTES 20
#define MTL_UDP_STD_LIMIT_BYTES 1460 /* ST 2110-10 §6.3, the UDP header included */

/* TX packet table entry, one per slot. */
struct mtl_pkt_tx {
  MTL_ADDR(uint8_t) data; /* the slot (SPLIT: its header slot; the payload slot is slot i
                             of plane 1); written by acquire, read-only to the application */
  uint16_t len;     /* bytes in the slot (SPLIT: payload bytes); 0 = skip the slot */
  uint16_t hdr_len; /* SPLIT: bytes in the header slot */
  uint32_t reserved;
};

/* RX packet table entry flags */
#define MTL_PKTE_GAP_BEFORE 0x1u /* `gap` sequence numbers are missing before this packet */
#define MTL_PKTE_UNIT_START 0x2u /* timestamp differs from the previous packet's */
#define MTL_PKTE_MARKER 0x4u
#define MTL_PKTE_REDUNDANT 0x8u /* NO_DEDUP: the other leg's copy */
#define MTL_PKTE_ARRIVAL_VALID 0x10u
#define MTL_PKTE_HW_ARRIVAL 0x20u
#define MTL_PKTE_HDR_EXT 0x40u /* CSRCs or an RFC 8285 extension follow the fixed header */
/* RX packet table entry, one per delivered packet. */
struct mtl_pkt_rx {
  MTL_ADDR(uint8_t) data;    /* the RTP header (Ethernet with INCLUDE_L2) */
  MTL_ADDR(uint8_t) payload; /* SPLIT: the payload; else NULL */
  uint16_t len;
  uint16_t payload_len;
  uint8_t leg;
  uint8_t flags; /* MTL_PKTE_* */
  uint16_t gap;  /* saturating */
  uint32_t seq;  /* extended for video, else 16-bit */
  uint32_t reserved;
  int64_t arrival_tai_ns; /* with MTL_PKTE_ARRIVAL_VALID */
};

/* Layout helpers (bindings reimplement them: they only compute addresses). */
static inline struct mtl_pkt_tx* mtl_pkt_tx_table(struct mtl_unit* u) {
  return (struct mtl_pkt_tx*)u->meta;
}
/* TX: where packet i of the chunk goes. */
static inline uint8_t* mtl_pkt_slot(struct mtl_unit* u, uint32_t i) {
  return (uint8_t*)mtl_pkt_tx_table(u)[i].data;
}
static inline const struct mtl_pkt_rx* mtl_pkt_rx_table(const struct mtl_unit* u) {
  return (const struct mtl_pkt_rx*)u->meta;
}

/* ---- RTP header (RFC 3550): byte layout and endian-safe accessors ------------------ */

struct mtl_rtp_hdr {
  uint8_t vpxcc;   /* version 2 in the top two bits */
  uint8_t mpt;     /* marker bit 7, payload type bits 0-6 */
  uint8_t seq[2];  /* big endian */
  uint8_t ts[4];   /* big endian */
  uint8_t ssrc[4]; /* big endian */
};
static inline uint32_t mtl_rtp_get_ts(const struct mtl_rtp_hdr* h) {
  return (uint32_t)h->ts[0] << 24 | (uint32_t)h->ts[1] << 16 | (uint32_t)h->ts[2] << 8 |
         h->ts[3];
}
static inline uint16_t mtl_rtp_get_seq(const struct mtl_rtp_hdr* h) {
  return (uint16_t)(h->seq[0] << 8 | h->seq[1]);
}
static inline void mtl_rtp_set(struct mtl_rtp_hdr* h, uint8_t pt, int marker, uint16_t seq,
                               uint32_t ts, uint32_t ssrc) {
  h->vpxcc = 0x80;
  h->mpt = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7f));
  h->seq[0] = (uint8_t)(seq >> 8);
  h->seq[1] = (uint8_t)seq;
  h->ts[0] = (uint8_t)(ts >> 24);
  h->ts[1] = (uint8_t)(ts >> 16);
  h->ts[2] = (uint8_t)(ts >> 8);
  h->ts[3] = (uint8_t)ts;
  h->ssrc[0] = (uint8_t)(ssrc >> 24);
  h->ssrc[1] = (uint8_t)(ssrc >> 16);
  h->ssrc[2] = (uint8_t)(ssrc >> 8);
  h->ssrc[3] = (uint8_t)ssrc;
}

/* ---- Payload headers of the essences, for packetisers and parsers (byte layouts) ----- */

/* RFC 4175 (ST 2110-20): after the RTP header, the extended sequence number, then one
   sample row data header per row segment. */
struct mtl_rfc4175_hdr {
  uint8_t ext_seq[2]; /* big endian: high 16 bits of the extended sequence number */
};
struct mtl_rfc4175_srd {
  uint8_t length[2]; /* big endian: bytes of this segment */
  uint8_t row[2];    /* big endian: F bit (second field) in bit 15, line number */
  uint8_t offset[2]; /* big endian: C bit (another SRD follows) in bit 15, pixel offset */
};
/* RFC 9134 (ST 2110-22, JPEG XS): the payload header after the RTP header. */
struct mtl_rfc9134_hdr {
  uint8_t bits[4]; /* T, K, L, I, F counter, SEP counter, P counter (big endian) */
};
/* RFC 8331 (ST 2110-40): after the RTP header, before the ANC packets. */
struct mtl_rfc8331_hdr {
  uint8_t ext_seq[2];     /* big endian */
  uint8_t length[2];      /* big endian: bytes of ANC data */
  uint8_t anc_count;      /* ANC packets */
  uint8_t f_reserved[3];  /* F field bits 7-6 */
};
/* ST 2110-41: the data item header after the RTP header. */
struct mtl_st41_hdr {
  uint8_t bits[4]; /* data item type (22 bits), K bit, data item length in words (big endian) */
};

MTL_SIZE_CHECK(mtl_pkt_tx, 16);
MTL_SIZE_CHECK(mtl_pkt_rx, 40);
MTL_SIZE_CHECK(mtl_rtp_hdr, 12);
MTL_SIZE_CHECK(mtl_rfc4175_hdr, 2);
MTL_SIZE_CHECK(mtl_rfc4175_srd, 6);
MTL_SIZE_CHECK(mtl_rfc9134_hdr, 4);
MTL_SIZE_CHECK(mtl_rfc8331_hdr, 8);
MTL_SIZE_CHECK(mtl_st41_hdr, 4);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_PACKET_H */
