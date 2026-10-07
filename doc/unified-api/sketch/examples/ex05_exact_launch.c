/* ex05 — send at the instant you choose: a unit's launch time is set apart from its media
   time, and its result says when it left. Needs: MS3 (EXACT MS2a; NOT_BEFORE MS1). */
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_options.h>

#include "ex_common.h"

void draw(struct mtl_unit* u, int64_t frame);
/* planned: the launch MTL scheduled; sent: the first packet (INT64_MIN: none); rtp: its
   RTP timestamp on the wire; margin_ns: INT64_MIN when not valid */
void launch_report(uint64_t id, uint32_t status, uint32_t reason, int64_t margin_ns,
                   int64_t planned, int64_t sent, uint32_t rtp, int from_nic);

/* base: a video TX config. INDEX mode: the media time comes from the content. exact = 1:
   MTL_SESSION_EXACT_LAUNCH, outside ST 2110-21 (MTL_INFO_NON_COMPLIANT). NIC launch times
   need MTL_INSTANCE_HW_TIMESTAMP; on an E830 caps.pacing = MTL_PACING_HW_LAUNCH. */
int open_launcher(mtl_instance_h mt, const struct mtl_session_config* base, int exact,
                  mtl_session_h* s) {
  const struct mtl_option hw = {MTL_OPT_HW_TIMESTAMPS, 0, MTL_REQ_PREFER, NULL};
  struct mtl_session_config sc = *base;
  sc.media_mode = MTL_MEDIA_INDEX;
  sc.flags |= MTL_SESSION_RESULTS | (exact ? MTL_SESSION_EXACT_LAUNCH : 0);
  sc.options = &hw;
  sc.option_count = 1;
  return mtl_session_open(mt, &sc, s);
}

/* Frame k (its media time M(k), its RTP) with its first packet at t (MTL_SUBMIT_EXACT, t
   at or after M(k): no packet before its media time), or at the first frame time whose
   first packet is at or after t (MTL_SUBMIT_NOT_BEFORE, on the schedule). */
int send_at(mtl_session_h s, int64_t k, int64_t t, uint64_t flag, uint64_t id) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire(s, &u, MTL_MS(20));
  if (ret < 0) return ret;
  draw(&u, k);
  u.media_index = k;
  u.launch_tai_ns = t;
  u.flags = flag;
  u.cookie = id;
  ret = mtl_tx_submit(s, &u); /* -MTL_ERANGE: LAUNCH_IN_PAST, BEYOND_HORIZON */
  return ret < 0 ? ex_fail("launch", ret) : 0;
}

/* Did each frame leave when asked? The full record has the planned launch and, with NIC
   timestamps, the launch seen on each leg; else sent_tai_ns is the software's. */
int check_launches(mtl_session_h s) {
  struct mtl_tx_result_full f[8];
  int n;
  while ((n = mtl_tx_reap_full(s, f, 8, 0)) > 0)
    for (int i = 0; i < n; i++) {
      const struct mtl_tx_result* r = &f[i].r;
      int nic = (f[i].detail_flags & MTL_TXF_OBSERVED_LEG0) != 0;
      int64_t sent = nic                               ? f[i].observed_first_tai_ns[0]
                     : (r->flags & MTL_TXR_SENT_VALID) ? r->sent_tai_ns
                                                       : INT64_MIN; /* DROPPED, FLUSHED */
      launch_report(r->cookie, r->status, r->reason,
                    (r->flags & MTL_TXR_MARGIN_VALID) ? r->margin_ns : INT64_MIN,
                    f[i].scheduled_tai_ns, sent, r->rtp, nic);
    }
  return n == -MTL_EAGAIN ? 0 : n;
}
