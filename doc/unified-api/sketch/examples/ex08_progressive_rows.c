/* ex08 — low-latency progressive TX (SDI-to-IP gateway): the first rows leave while the
   rest of the frame is still arriving. Session: a video TX config with sc.unit =
   MTL_UNIT_ROWS, sc.media_mode = MTL_MEDIA_INDEX and sc.video.troffset_us, so the frame
   leaves in the frame period it is captured in. Each submit of the same lease
   publishes rows. Needs: MS3 (rows units MS2a; INDEX and mtl_tx_get_next MS3). */
#include <mtl/experimental/mtl_sync.h>

#include "ex_common.h"

#define STEP 64u
void render_rows(struct mtl_unit* u, uint32_t first, uint32_t end);

int send_one_frame(mtl_session_h s) {
  struct mtl_unit u;
  struct mtl_tx_next cur;
  MTL_INIT(&u);
  int ret = mtl_tx_get_next(s, &cur, sizeof(cur)); /* which frame is being filled */
  if (ret >= 0) ret = mtl_tx_acquire(s, &u, MTL_MS(20));
  if (ret < 0) return ex_fail("acquire", ret);

  u.media_index = cur.next_media_index;
  for (uint32_t done = 0; done < u.plane[0].rows;) {
    uint32_t next = done + STEP < u.plane[0].rows ? done + STEP : u.plane[0].rows;
    render_rows(&u, done, next);
    u.used = next; /* rows [0, next) are final; used == rows ends the frame */
    /* a failed first submit returns the slot; a later failure ends the frame where its
       rows stopped (tx.rows_late) */
    ret = mtl_tx_submit(s, &u);
    if (ret < 0) return ex_fail("submit", ret);
    done = next;
  }
  return 0;
}
