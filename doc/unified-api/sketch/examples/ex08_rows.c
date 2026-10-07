/* ex08 — rows: a frame's first rows leave while the rest is still arriving (an SDI-to-IP
   gateway), and a receiver reads rows before the frame is complete. Needs: MS3. */
#include <mtl/experimental/mtl_sync.h>

#include "ex_common.h"

#define STEP 64u
void render_rows(struct mtl_unit* u, uint32_t first, uint32_t end); /* the SDI input */
void consume_rows(const struct mtl_unit* u, uint32_t first, uint32_t end);
void report(const char* what, int64_t value);

/* Rows after their deadline end the frame there (tx.rows_late), and the result says so;
   this says by how much. */
static void report_if_late(mtl_instance_h mt, mtl_session_h s, int64_t k, uint32_t row) {
  int64_t due, now;
  if (mtl_tx_row_deadline(s, k, row, &due) >= 0 &&
      mtl_time_now(mt, &now, NULL, NULL) >= 0 && now > due)
    report("row late ns", now - due);
}

/* TX: unit MTL_UNIT_ROWS, media_mode INDEX, video.troffset_us. The index is the next
   frame that can still go: a gateway that runs late stamps a later frame, so it declares
   tsmode NEW; one that keeps the SDI frame's instant derives the index from its input. */
int send_one_frame(mtl_instance_h mt, mtl_session_h s) {
  struct mtl_unit u;
  struct mtl_tx_next nx;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire(s, &u, MTL_FOREVER); /* results off */
  if (ret < 0) return ret;
  ret = mtl_tx_get_next(s, &nx, sizeof(nx)); /* after the wait: the frame being filled */
  if (ret < 0) {
    mtl_tx_release(s, u.lease);
    return ret;
  }

  u.media_index = nx.next_media_index;
  for (uint32_t done = 0; done < u.plane[0].rows;) {
    uint32_t next = done + STEP;
    if (next > u.plane[0].rows) next = u.plane[0].rows;
    render_rows(&u, done, next);
    report_if_late(mt, s, u.media_index, next - 1);
    u.used = next; /* rows [0, next) are final; used == rows ends the frame */
    ret = mtl_tx_submit(s, &u);
    if (ret < 0) return ex_fail("submit", ret);
    done = next;
  }
  return 0;
}

/* RX: rows as they complete, STEP at a time. A unit that ends short (lost packets, its
   due time) returns fewer rows than asked; its final status is mtl_rx_get_detail's. */
int receive_one_frame(mtl_session_h s) {
  struct mtl_unit u;
  uint32_t have = 0;
  MTL_INIT(&u);
  int ret = mtl_rx_dequeue(s, &u, MTL_FOREVER);
  if (ret < 0) return ret;
  uint32_t rows_done = u.used; /* rows units: used counts the rows complete */
  while (ret >= 0 && rows_done > have) {
    consume_rows(&u, have, rows_done);
    have = rows_done;
    if (have == u.plane[0].rows || !(u.flags & MTL_UNITF_PARTIAL)) break;
    uint32_t want = have + STEP;
    if (want > u.plane[0].rows) want = u.plane[0].rows;
    ret = mtl_rx_wait_rows(s, u.lease, want, &rows_done, MTL_FOREVER);
  }
  int r = mtl_rx_release(s, u.lease);
  return ret < 0 ? ret : r;
}
