/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../mt_log.h"

typedef union fmd_udw_10_6e {
  struct {
    uint16_t : 6;
    uint16_t udw : 10;
  };
  uint16_t val;
} __attribute__((__packed__)) fmd_udw_10_6e_t;

typedef union fmd_udw_2e_10_4e {
  struct {
    uint16_t : 4;
    uint16_t udw : 10;
    uint16_t : 2;
  };
  uint16_t val;
} __attribute__((__packed__)) fmd_udw_2e_10_4e_t;

typedef union fmd_udw_4e_10_2e {
  struct {
    uint16_t : 2;
    uint16_t udw : 10;
    uint16_t : 4;
  };
  uint16_t val;
} __attribute__((__packed__)) fmd_udw_4e_10_2e_t;

typedef union fmd_udw_6e_10 {
  struct {
    uint16_t udw : 10;
    uint16_t : 6;
  };
  uint16_t val;
} __attribute__((__packed__)) fmd_udw_6e_10_t;

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

static uint16_t st_ntohs(uint8_t* data) {
  return ((data[0] << 8) | (data[1]));
}

static uint16_t get_10bit_udw(int idx, uint8_t* data) {
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
      fmd_udw_10_6e_t val10;
      val10.val = val;
      udw = val10.udw;
      break;
    }
    case 2: {
      fmd_udw_2e_10_4e_t val10;
      val10.val = val;
      udw = val10.udw;
      break;
    }
    case 4: {
      fmd_udw_4e_10_2e_t val10;
      val10.val = val;
      udw = val10.udw;
      break;
    }
    case 6: {
      fmd_udw_6e_10_t val10;
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
      fmd_udw_10_6e_t val10;
      val10.val = val;
      val10.udw = udw;
      val = val10.val;
      break;
    }
    case 2: {
      fmd_udw_2e_10_4e_t val10;
      val10.val = val;
      val10.udw = udw;
      val = val10.val;
      break;
    }
    case 4: {
      fmd_udw_4e_10_2e_t val10;
      val10.val = val;
      val10.udw = udw;
      val = val10.val;
      break;
    }
    case 6: {
      fmd_udw_6e_10_t val10;
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

uint16_t st41_get_udw(uint32_t idx, uint8_t* data) {
  return get_10bit_udw(idx, data);
}

void st41_set_udw(uint32_t idx, uint16_t udw, uint8_t* data) {
  set_10bit_udw(idx, udw, data);
}

uint16_t st41_calc_checksum(uint32_t data_num, uint8_t* data) {
  uint16_t chks = 0, udw;
  for (uint32_t i = 0; i < data_num; i++) {
    udw = get_10bit_udw(i, data);
    chks += udw;
  }
  chks &= 0x1ff;
  chks = (~((chks << 1)) & 0x200) | chks;

  return chks;
}

uint16_t st41_add_parity_bits(uint16_t val) {
  return get_parity_bits(val) | (val & 0xFF);
}

int st41_check_parity_bits(uint16_t val) {
  return val == st41_add_parity_bits(val & 0xFF);
}