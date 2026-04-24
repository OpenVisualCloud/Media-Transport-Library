/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Bridge for tv_frame_payload_cross_page: constructs an
 * st_frame_trans with a synthetic page table (caller-described) and
 * asks whether [offset, offset+len) crosses an iova-discontiguous
 * page boundary.
 *
 * Pages are described in the caller's coordinates: a va offset from
 * an internal base buffer + a fake iova + a length. We build the
 * page_table[] with addresses pointing into a real allocation so the
 * "addr in page" comparisons in tv_frame_get_offset_iova are
 * legitimate pointer comparisons.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../../lib/src/st2110/st_tx_video_session.c"

struct test_page {
  size_t va_offset; /* offset from caller-visible base */
  uint64_t iova;    /* opaque integer; not interpreted by the function */
  size_t len;
};

bool test_payload_cross_page(const struct test_page* pages, int n_pages, size_t offset,
                             size_t len) {
  /* Find the total VA range we need to cover. */
  size_t total = 0;
  for (int i = 0; i < n_pages; i++) {
    size_t end = pages[i].va_offset + pages[i].len;
    if (end > total) total = end;
  }
  /* Always allocate at least 1 byte so calloc() returns non-NULL even
   * for the page_table_len == 0 path. */
  if (total == 0) total = 1;
  void* base = calloc(1, total);

  struct st_page_info* table = NULL;
  if (n_pages > 0) {
    table = calloc(n_pages, sizeof(*table));
    for (int i = 0; i < n_pages; i++) {
      table[i].addr = (char*)base + pages[i].va_offset;
      table[i].iova = (rte_iova_t)pages[i].iova;
      table[i].len = pages[i].len;
    }
  }

  struct st_frame_trans frame_info;
  memset(&frame_info, 0, sizeof(frame_info));
  frame_info.addr = base;
  frame_info.iova = 0;
  frame_info.page_table = table;
  frame_info.page_table_len = (uint16_t)n_pages;
  frame_info.idx = 0;

  struct st_tx_video_session_impl s;
  memset(&s, 0, sizeof(s));
  s.idx = 0;

  bool result = tv_frame_payload_cross_page(&s, &frame_info, offset, len);

  free(table);
  free(base);
  return result;
}
