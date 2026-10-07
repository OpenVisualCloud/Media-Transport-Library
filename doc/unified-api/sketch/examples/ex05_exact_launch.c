/* ex05 — send at the instant you choose: a unit's launch time is set apart from its media
   time, and its result says when it left. Needs: MS3. */
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_options.h>

#include "ex_common.h"

void draw(struct mtl_unit* u, int64_t frame);
/* r: the outcome (status, reason, cookie, rtp; margin_ns with MTL_TXR_MARGIN_VALID);
   planned: the launch MTL scheduled; sent: the first packet (INT64_MIN: never sent), a
   NIC timestamp when from_nic */
void launch_report(const struct mtl_tx_result* r, int64_t planned, int64_t sent,
                   int from_nic);

/* base: a video TX config. INDEX mode: the media time comes from the content. exact = 1:
   MTL_SESSION_EXACT_LAUNCH, outside ST 2110-21 (MTL_INFO_NON_COMPLIANT). The NIC's own
   send times need MTL_INSTANCE_HW_TIMESTAMP and the option below. A launch in hardware
   needs an E830 (caps.pacing MTL_PACING_HW_LAUNCH); elsewhere MTL paces it. */
int open_launcher(mtl_instance_h mt, const struct mtl_session_config* base, int exact,
                  mtl_session_h* s) {
  const struct mtl_option hw = {.key = MTL_OPT_HW_TIMESTAMPS, .value = MTL_REQ_PREFER};
  struct mtl_session_config sc = *base;
  sc.media_mode = MTL_MEDIA_INDEX;
  sc.flags |= MTL_SESSION_RESULTS | (exact ? MTL_SESSION_EXACT_LAUNCH : 0);
  sc.options = &hw;
  sc.option_count = 1;
  return mtl_session_open(mt, &sc, s);
}

/* Did each frame leave when asked? The full record adds the planned launch. */
int check_launches(mtl_session_h s) {
  struct mtl_tx_result_full f[8];
  int n;
  while ((n = mtl_tx_reap_full(s, f, 8, 0)) > 0)
    for (int i = 0; i < n; i++) {
      const struct mtl_tx_result* r = &f[i].r;
      int64_t sent = INT64_MIN; /* DROPPED, FLUSHED: never sent */
      if (r->flags & MTL_TXR_SENT_VALID) sent = r->sent_tai_ns;
      launch_report(r, f[i].scheduled_tai_ns, sent, (r->flags & MTL_TXR_SENT_HW) != 0);
    }
  return n == -MTL_EAGAIN ? 0 : n;
}

/* Frame k with its first packet at t. MTL_SUBMIT_EXACT: exactly at t, which is at or
   after the frame's media time. MTL_SUBMIT_NOT_BEFORE: at the first frame time whose
   first packet is at or after t. */
int send_at(mtl_session_h s, int64_t k, int64_t t, uint64_t mode, uint64_t id) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = check_launches(s); /* read first: then acquire never waits on results */
  if (ret >= 0) ret = mtl_tx_acquire(s, &u, MTL_FOREVER);
  if (ret < 0) return ret;
  draw(&u, k);
  u.media_index = k;
  u.launch_tai_ns = t;
  u.flags |= mode;
  u.cookie = id;
  ret = mtl_tx_submit(s, &u); /* -MTL_ERANGE: LAUNCH_IN_PAST, BEYOND_HORIZON */
  return ret < 0 ? ex_fail("launch", ret) : 0;
}
