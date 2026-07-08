/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <errno.h>
#include <string.h>

#include "../mt_log.h"
#include "../mt_platform.h"
#include "st40_api.h"

typedef union anc_udw_10_6e {
  struct {
    uint16_t : 6;
    uint16_t udw : 10;
  };
  uint16_t val;
} __attribute__((__packed__)) anc_udw_10_6e_t;

typedef union anc_udw_2e_10_4e {
  struct {
    uint16_t : 4;
    uint16_t udw : 10;
    uint16_t : 2;
  };
  uint16_t val;
} __attribute__((__packed__)) anc_udw_2e_10_4e_t;

typedef union anc_udw_4e_10_2e {
  struct {
    uint16_t : 2;
    uint16_t udw : 10;
    uint16_t : 4;
  };
  uint16_t val;
} __attribute__((__packed__)) anc_udw_4e_10_2e_t;

typedef union anc_udw_6e_10 {
  struct {
    uint16_t udw : 10;
    uint16_t : 6;
  };
  uint16_t val;
} __attribute__((__packed__)) anc_udw_6e_10_t;

static uint16_t parity_tab[] = {
    //              0       1       2       3       4       5       6       7       8 9 A
    //              B       C       D       E       F
    /* 0 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    0x0100,         0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    /* 1 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    0x0200,         0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    /* 2 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    0x0200,         0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    /* 3 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    0x0100,         0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    /* 4 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    0x0200,         0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    /* 5 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    0x0100,         0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    /* 6 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    0x0100,         0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    /* 7 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    0x0200,         0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    /* 8 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    0x0200,         0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    /* 9 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    0x0100,         0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    /* A */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    0x0100,         0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    /* B */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    0x0200,         0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    /* C */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    0x0100,         0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    /* D */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    0x0200,         0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    /* E */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
    0x0200,         0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    /* F */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
    0x0100,         0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
};

static inline uint16_t get_parity_bits(uint16_t val) {
  return parity_tab[val & 0xFF];
}

static uint16_t st_ntohs(const uint8_t* data) {
  return ((data[0] << 8) | (data[1]));
}

static uint16_t get_10bit_udw(int idx, const uint8_t* data) {
  int byte_offset, bit_offset;
  int total_bits_offset = idx * 10; /*10 bit per field */
  byte_offset = total_bits_offset / 8;
  bit_offset = total_bits_offset % 8;
  data += byte_offset;
  //  host to network conversion without load from misaligned address
  uint16_t val = st_ntohs(data);
  uint16_t udw = 0;

  switch (bit_offset) {
    case 0: {
      anc_udw_10_6e_t val10;
      val10.val = val;
      udw = val10.udw;
      break;
    }
    case 2: {
      anc_udw_2e_10_4e_t val10;
      val10.val = val;
      udw = val10.udw;
      break;
    }
    case 4: {
      anc_udw_4e_10_2e_t val10;
      val10.val = val;
      udw = val10.udw;
      break;
    }
    case 6: {
      anc_udw_6e_10_t val10;
      val10.val = val;
      udw = val10.udw;
      break;
    }
  }
  return udw;
}

static void set_10bit_udw(int idx, uint16_t udw, uint8_t* data) {
  int byte_offset, bit_offset;
  int total_bits_offset = idx * 10; /*10 bit per field */
  byte_offset = total_bits_offset / 8;
  bit_offset = total_bits_offset % 8;
  data += byte_offset;
  //  host to network conversion without load from misaligned address
  uint16_t val = st_ntohs(data);
  switch (bit_offset) {
    case 0: {
      anc_udw_10_6e_t val10;
      val10.val = val;
      val10.udw = udw;
      val = val10.val;
      break;
    }
    case 2: {
      anc_udw_2e_10_4e_t val10;
      val10.val = val;
      val10.udw = udw;
      val = val10.val;
      break;
    }
    case 4: {
      anc_udw_4e_10_2e_t val10;
      val10.val = val;
      val10.udw = udw;
      val = val10.val;
      break;
    }
    case 6: {
      anc_udw_6e_10_t val10;
      val10.val = val;
      val10.udw = udw;
      val = val10.val;
      break;
    }
  }
  //  host to network conversion without store to misaligned address
  data[0] = (uint8_t)((val & 0xFF00) >> 8);
  data[1] = (uint8_t)(val & 0xFF);
}

uint16_t st40_get_udw(uint32_t idx, const uint8_t* data) {
  return get_10bit_udw(idx, data);
}

void st40_set_udw(uint32_t idx, uint16_t udw, uint8_t* data) {
  set_10bit_udw(idx, udw, data);
}

uint16_t st40_calc_checksum(uint32_t data_num, const uint8_t* data) {
  uint16_t chks = 0, udw;
  for (uint32_t i = 0; i < data_num; i++) {
    udw = get_10bit_udw(i, data);
    chks += udw;
  }
  chks &= 0x1ff;
  chks = (~((chks << 1)) & 0x200) | chks;

  return chks;
}

uint32_t st40_rfc8331_payload_bytes(uint16_t udw_size) {
  /* 10-bit words: DID, SDID, DC, UDW[udw_size], checksum. */
  uint32_t total_bits = (uint32_t)(3 + udw_size + 1) * 10;
  uint32_t total_size = (total_bits + 7) / 8;
  total_size = (total_size + 3) & ~0x3U;
  return (uint32_t)(sizeof(struct st40_rfc8331_payload_hdr) - 4) + total_size;
}

void st40_rfc8331_rtp_hdr_bswap(struct st40_rfc8331_rtp_hdr* hdr) {
  hdr->swapped_first_hdr_chunk = htonl(hdr->swapped_first_hdr_chunk);
}

void st40_rfc8331_payload_hdr_bswap(struct st40_rfc8331_payload_hdr* hdr) {
  hdr->swapped_first_hdr_chunk = htonl(hdr->swapped_first_hdr_chunk);
  hdr->swapped_second_hdr_chunk = htonl(hdr->swapped_second_hdr_chunk);
}

int st40_rfc8331_encode_packet(uint8_t* buf, uint32_t room, const struct st40_meta* meta,
                               const uint8_t* udw_in, uint32_t* written) {
  /* TX skips this: TX_ANC_TEST_APPLY_PARITY needs direct primitive access. */
  uint16_t udw_size = meta->udw_size;
  if (udw_size > 0xFF) return -EINVAL;
  uint32_t need = st40_rfc8331_payload_bytes(udw_size);
  if (room < need) return -ENOSPC;

  /* Zero the body so checksum padding bits are deterministic. */
  memset(buf, 0, need);

  struct st40_rfc8331_payload_hdr* ph = (struct st40_rfc8331_payload_hdr*)buf;
  ph->first_hdr_chunk.c = meta->c;
  ph->first_hdr_chunk.line_number = meta->line_number;
  ph->first_hdr_chunk.horizontal_offset = meta->hori_offset;
  ph->first_hdr_chunk.s = meta->s;
  ph->first_hdr_chunk.stream_num = meta->stream_num;
  ph->second_hdr_chunk.did = st40_add_parity_bits(meta->did);
  ph->second_hdr_chunk.sdid = st40_add_parity_bits(meta->sdid);
  ph->second_hdr_chunk.data_count = st40_add_parity_bits(udw_size);

  st40_rfc8331_payload_hdr_bswap(ph);

  uint8_t* udw_dst = (uint8_t*)&ph->second_hdr_chunk;
  for (uint16_t i = 0; i < udw_size; i++) {
    st40_set_udw(i + 3, st40_add_parity_bits(udw_in[i]), udw_dst);
  }
  uint16_t checksum = st40_calc_checksum(3 + udw_size, udw_dst);
  st40_set_udw(udw_size + 3, checksum, udw_dst);

  *written = need;
  return 0;
}

enum st40_rfc8331_decode_result st40_rfc8331_decode_packet(
    const uint8_t* buf, uint32_t room, struct st40_meta* meta, uint8_t* udw_out,
    uint32_t udw_cap, uint32_t* consumed) {
  if (room < sizeof(struct st40_rfc8331_payload_hdr))
    return ST40_RFC8331_DECODE_SHORT_BUFFER;

  /* Read header without mutating caller memory: copy + bswap a local. */
  struct st40_rfc8331_payload_hdr hdr_local =
      *(const struct st40_rfc8331_payload_hdr*)buf;
  st40_rfc8331_payload_hdr_bswap(&hdr_local);

  if (!st40_check_parity_bits(hdr_local.second_hdr_chunk.did) ||
      !st40_check_parity_bits(hdr_local.second_hdr_chunk.sdid) ||
      !st40_check_parity_bits(hdr_local.second_hdr_chunk.data_count))
    return ST40_RFC8331_DECODE_PARITY_FAIL;

  uint16_t udw_size = hdr_local.second_hdr_chunk.data_count & 0xFF;
  uint32_t need = st40_rfc8331_payload_bytes(udw_size);
  if (room < need) return ST40_RFC8331_DECODE_SHORT_BUFFER;
  if (udw_out && udw_size > udw_cap) return ST40_RFC8331_DECODE_SHORT_BUFFER;

  /* UDW/checksum reads need wire-order layout; buf is already wire-order. */
  const uint8_t* udw_src =
      (const uint8_t*)&((const struct st40_rfc8331_payload_hdr*)buf)->second_hdr_chunk;

  for (uint16_t i = 0; i < udw_size; i++) {
    uint16_t udw = st40_get_udw(i + 3, udw_src);
    if (!st40_check_parity_bits(udw)) return ST40_RFC8331_DECODE_PARITY_FAIL;
    if (udw_out) udw_out[i] = (uint8_t)(udw & 0xFF);
  }

  uint16_t checksum_wire = st40_get_udw(udw_size + 3, udw_src);
  uint16_t checksum_calc = st40_calc_checksum(3 + udw_size, udw_src);
  if (checksum_wire != checksum_calc) return ST40_RFC8331_DECODE_CHECKSUM_FAIL;

  meta->c = hdr_local.first_hdr_chunk.c;
  meta->line_number = hdr_local.first_hdr_chunk.line_number;
  meta->hori_offset = hdr_local.first_hdr_chunk.horizontal_offset;
  meta->s = hdr_local.first_hdr_chunk.s;
  meta->stream_num = hdr_local.first_hdr_chunk.stream_num;
  meta->did = hdr_local.second_hdr_chunk.did & 0xFF;
  meta->sdid = hdr_local.second_hdr_chunk.sdid & 0xFF;
  meta->udw_size = udw_size;
  meta->udw_offset = 0;

  *consumed = need;
  return ST40_RFC8331_DECODE_OK;
}

uint16_t st40_add_parity_bits(uint16_t val) {
  return get_parity_bits(val) | (val & 0xFF);
}

int st40_check_parity_bits(uint16_t val) {
  return val == st40_add_parity_bits(val & 0xFF);
}