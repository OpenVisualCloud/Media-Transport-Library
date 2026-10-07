/* ex04 — buffer ahead, then start at an instant: frames are queued before the first is
   due, and the pool bounds how far ahead the producer runs. Needs: MS3. */
#include <mtl/experimental/mtl_sync.h>

#include "ex_common.h"

#define DEPTH 8 /* frames queued ahead: 133 ms at 59.94 */

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

static int reap(mtl_session_h s) { /* margin_ns: the head start each frame had */
  struct mtl_tx_result r[DEPTH];
  int n;
  while ((n = mtl_tx_reap(s, r, DEPTH, 0)) > 0)
    for (int i = 0; i < n; i++)
      if (r[i].flags & MTL_TXR_MARGIN_VALID)
        report(mtl_reason_name(r[i].reason), r[i].margin_ns);
  return n == -MTL_EAGAIN ? 0 : n;
}

/* Queues frames from *next until the pool is full (0) or the source ends (1). A producer
   that fell behind skips to the first index that can still go. Interlaced: indices count
   fields, even = first field, and parity follows submission order. */
static int fill(mtl_session_h s, int64_t* next) {
  struct mtl_unit u;
  struct mtl_tx_next n;
  MTL_INIT(&u);
  for (;;) {
    int ret = mtl_tx_get_next(s, &n, sizeof(n));
    if (ret >= 0 && n.next_media_index > *next) {
      skip_source(n.next_media_index - *next);
      *next = n.next_media_index;
    }
    if (ret >= 0) ret = mtl_tx_acquire(s, &u, 0); /* -MTL_EAGAIN: DEPTH frames queued */
    if (ret == -MTL_EAGAIN) return reap(s);       /* or results unread: blocked_on */
    if (ret < 0) return ret;
    if (render(u.plane[0].addr, u.plane[0].stride, *next) != 0) {
      mtl_tx_release(s, u.lease);
      return 1;
    }
    u.media_index = (*next)++;
    if ((ret = mtl_tx_submit(s, &u)) < 0) return ret;
  }
}

/* Now: T0 is the first frame time after now + the lead + preroll_ns, the time to queue
   the first frames; ARMED until then, and next_media_index is T0's index. */
int start_now(mtl_session_h s, int64_t frame_ns, int64_t* next) {
  struct mtl_when when = {.kind = MTL_NOW, .preroll_ns = DEPTH * frame_ns + MTL_MS(10)};
  struct mtl_tx_next n;
  int ret = mtl_session_start(&s, 1, &when, NULL);
  if (ret >= 0) ret = mtl_tx_get_next(s, &n, sizeof(n));
  if (ret < 0) return ret;
  *next = n.next_media_index;
  return fill(s, next);
}

/* At an instant every process of the programme was given (one per essence): a whole
   number of 1.001 s (seconds for integer rates) is exact for every 1001-rate frame and 48
   or 96 kHz sample. unit_rate: the index rate (fields when interlaced; {48000, 1} for
   audio). */
int start_at(mtl_session_h s, int64_t t_start, struct mtl_rational unit_rate,
             int64_t* next) {
  struct mtl_when when = {.kind = MTL_AT_TAI, .value = t_start};
  int ret = mtl_epoch_index_at(t_start, unit_rate, next); /* exact on such a t_start */
  if (ret >= 0) ret = fill(s, next);                      /* queued while CREATED */
  return ret < 0 ? ret : mtl_session_start(&s, 1, &when, NULL); /* START_IN_PAST */
}

/* Keeps DEPTH frames ahead, and says how much media is queued ahead of now. */
int run(mtl_instance_h mt, mtl_session_h s, int64_t next) {
  int ret = 0;
  while (ret == 0 && g_running) {
    struct mtl_tx_next n;
    int64_t now;
    ret = mtl_session_wait(s, MTL_WAIT_ACQUIRE | MTL_WAIT_RESULTS, MTL_MS(100));
    if (ret >= 0 || ret == -MTL_EAGAIN) ret = fill(s, &next);
    if (ret == 0 && mtl_tx_get_next(s, &n, sizeof(n)) >= 0 &&
        mtl_time_now(mt, &now, NULL, NULL) >= 0)
      report("queued ahead ns", n.next_media_tai_ns - now);
  }
  if (ret < 0) ex_fail("timed", ret);
  mtl_session_close(s, MTL_SEC(1)); /* sends what is queued, then retires */
  return ret < 0 ? ret : 0;
}
