/* ex04 — buffer ahead, then start at an instant: frames are queued before the first is
   due, and the pool bounds how far ahead the producer runs. Call open_timed(), then
   start_now() or start_at(), then run(). Needs: MS3. */
#include <mtl/experimental/mtl_sync.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

#define DEPTH 8                /* frames queued ahead: 133 ms at 59.94 */
#define SUBMIT_TIME MTL_MS(10) /* the time to submit the first DEPTH frames */
#define SOURCE_ENDED 1

int render(void* addr, uint32_t stride, int64_t index); /* 1 = the source ended */
void skip_source(int64_t frames);                       /* frames that can no longer go */
void report(const char* what, int64_t value);

/* INDEX mode: each frame names its index on the epoch; the pool is the buffer. Beyond 1 s
   of media also raise MTL_OPT_HORIZON_NS (a frame past it is -MTL_ERANGE). */
int open_timed(mtl_instance_h mt, const struct mtl_session_config* base,
               mtl_session_h* s) {
  struct mtl_session_config sc = *base;
  sc.media_mode = MTL_MEDIA_INDEX;
  sc.flags |= MTL_SESSION_RESULTS;
  sc.pool_count = DEPTH;
  return mtl_session_create(mt, &sc, s);
}

/* Queues frames from *next while a slot is free: 0 once DEPTH frames are queued (or
   results wait unread: run() reaps them), SOURCE_ENDED, or an error. Interlaced: indices
   count fields, even = first field. */
static int fill(mtl_session_h s, int64_t* next) {
  struct mtl_unit u;
  int ret;
  MTL_INIT(&u);
  while ((ret = mtl_tx_acquire(s, &u, 0)) == 0) {
    if (render(u.plane[0].addr, u.plane[0].stride, *next) != 0) {
      mtl_tx_release(s, u.lease);
      return SOURCE_ENDED;
    }
    u.media_index = (*next)++;
    if ((ret = mtl_tx_submit(s, &u)) < 0) return ret;
  }
  return ret == -MTL_EAGAIN ? 0 : ret;
}

/* Now: T0 is the first frame time at or after now + MTL's lead + preroll_ns, so the
   first DEPTH frames are queued before it; the session is ARMED until T0. unit_ns: a
   frame (mtl_frame_ns), or a field when interlaced. */
int start_now(mtl_session_h s, int64_t unit_ns, int64_t* next) {
  struct mtl_when when = {.kind = MTL_NOW, .preroll_ns = DEPTH * unit_ns + SUBMIT_TIME};
  struct mtl_tx_next nx;
  int ret = mtl_session_start(&s, 1, &when, NULL);
  if (ret >= 0) ret = mtl_tx_get_next(s, &nx, sizeof(nx)); /* ARMED: T0's index */
  if (ret < 0) return ret;
  *next = nx.next_media_index;
  return fill(s, next);
}

/* At an instant every process of the programme was given (one per essence). Give them
   the same t_start, a multiple of 1.001 s (1 s for integer rates): it then falls exactly
   on a frame and on a sample. unit_rate: the index rate (fields when interlaced;
   {48000, 1} for audio). The frames are queued while the session is CREATED. */
int start_at(mtl_session_h s, int64_t t_start, struct mtl_rational unit_rate,
             int64_t* next) {
  struct mtl_when when = {.kind = MTL_AT_TAI, .value = t_start};
  int ret = mtl_epoch_index_at(t_start, unit_rate, next); /* exact on such a t_start */
  int filled = ret < 0 ? ret : fill(s, next);             /* or SOURCE_ENDED */
  if (filled < 0) return filled;
  ret = mtl_session_start(&s, 1, &when, NULL); /* START_IN_PAST: t_start has gone */
  return ret < 0 ? ret : filled;
}

/* margin_ns: the head start each frame had */
static void on_result(void* priv, const struct mtl_tx_result* r) {
  (void)priv;
  if (r->flags & MTL_TXR_MARGIN_VALID) report(mtl_reason_name(r->reason), r->margin_ns);
}

/* A producer that fell behind skips to the first index that can still go. */
static int catch_up(mtl_session_h s, int64_t* next) {
  struct mtl_tx_next nx;
  int ret = mtl_tx_get_next(s, &nx, sizeof(nx));
  if (ret >= 0 && nx.next_media_index > *next) {
    skip_source(nx.next_media_index - *next);
    *next = nx.next_media_index;
  }
  return ret;
}

/* Keeps DEPTH frames ahead until the source ends, and says how much media is queued
   ahead of now. */
int run(mtl_instance_h mt, mtl_session_h s, int64_t next) {
  int ret = 0;
  while (ret == 0) {
    struct mtl_tx_next nx;
    int64_t now;
    ret = mtl_session_wait(s, MTL_WAIT_ACQUIRE | MTL_WAIT_RESULTS, MTL_FOREVER);
    if (ret >= 0)
      ret = mtl_tx_reap_each(s, on_result, NULL); /* unread, they hold slots */
    if (ret >= 0) ret = catch_up(s, &next);
    if (ret >= 0) ret = fill(s, &next);
    if (ret == 0 && mtl_tx_get_next(s, &nx, sizeof(nx)) >= 0 &&
        mtl_time_now(mt, &now, NULL, NULL) >= 0)
      report("queued ahead ns", nx.next_media_tai_ns - now);
  }
  if (ret < 0 && ret != -MTL_ECANCELED) ex_fail("timed", ret);
  mtl_session_close(s, MTL_SEC(1)); /* sends what is queued, then retires */
  return ret < 0 && ret != -MTL_ECANCELED ? ret : 0;
}
