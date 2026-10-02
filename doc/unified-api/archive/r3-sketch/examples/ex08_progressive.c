/* 10 §10 — progressive (line) TX (P4): submit the first rows, publish the rest. Session:
   sc.unit = MTL_UNIT_ROWS, timing.source_kind = MTL_SOURCE_GATEWAY,
   timing.media_mode = MTL_MEDIA_INDEX, vc.troffset_ns set (< TFRAME, signalled as TROFF).
 */
#include "ex_common.h"

#define STEP 64u

void render_rows(const struct mtl_buffer_view* v, uint32_t first, uint32_t end);

int send_one_frame(mtl_session_h s) {
  mtl_lease_h lease;
  struct mtl_buffer_view v;
  struct mtl_slot_hint hint;
  mtl_buffer_view_init(&v);
  mtl_slot_hint_init(&hint);
  int ret = mtl_tx_acquire(s, &lease, &v, &hint, MTL_MS(20)); /* the slot being filled */
  if (ret < 0) return ex_fail("acquire", ret);

  const uint32_t rows = v.plane[0].rows;
  render_rows(&v, 0, STEP);
  struct mtl_tx_submission sub;
  mtl_tx_submission_init(&sub);
  sub.media_index = hint.next_media_index;
  sub.ready_rows = STEP;
  /* the lease stays current for publish until FINAL */
  ret = mtl_tx_submit(s, lease, &sub);
  if (ret < 0) {
    mtl_tx_release(s, lease);
    return ex_fail("submit", ret);
  }
  for (uint32_t done = STEP; done < rows;) {
    uint32_t next = done + STEP > rows ? rows : done + STEP;
    render_rows(&v, done, next);
    ret = mtl_tx_publish(s, lease, next); /* next == rows is FINAL */
    if (ret < 0) return ex_fail("publish", ret);
    done = next;
  }
  return 0;
}
