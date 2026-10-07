/* ex03 — senders in the application's own epoll loop, reading a result per frame: one
   queue's descriptor stands for all the sessions, beside the source's eventfd. A report
   disarms its session: the loop serves it with timeout-0 calls, then arms its results
   and, only while a source frame waits, a free slot, so it never spins on free slots. It
   sleeps only after mtl_queue_wait() returned -MTL_EAGAIN. s[i]: started TX sessions
   with MTL_SESSION_RESULTS. Needs: MS2 (MS2a: queues). */
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "ex_common.h"

/* The source (decoder threads, socket readers): one eventfd (EFD_NONBLOCK) it writes
   after it queues a frame for any session, and a queue of frames per session. */
int source_eventfd(void);
int source_frame_waiting(int i);                               /* 1: a frame for s[i] */
int next_source_frame(int i, uint64_t* id, const void** data); /* 0 = one taken */
void fill(struct mtl_unit* u, const void* data);
void source_frame_done(int i, uint64_t id, uint32_t status, int64_t margin_ns);

/* Why drain() stopped: the example's own values, not library names. */
enum { EX_DRAINED_NO_FRAME = 0, EX_DRAINED_NO_SLOT = 1 };

/* Results, then a slot for each frame that waits: the slot first, so no frame is taken
   without a place to go. */
static int drain(mtl_session_h s, int i) {
  struct mtl_tx_result r[16];
  struct mtl_unit u;
  int n;
  MTL_INIT(&u);
  while ((n = mtl_tx_reap(s, r, 16, 0)) > 0) /* in submission order */
    for (int k = 0; k < n; k++)
      source_frame_done(i, r[k].cookie, r[k].status, r[k].margin_ns);
  if (n != -MTL_EAGAIN) return n;
  while (source_frame_waiting(i)) {
    uint64_t id;
    const void* data;
    int ret = mtl_tx_acquire(s, &u, 0);
    if (ret == -MTL_EAGAIN) return EX_DRAINED_NO_SLOT;
    if (ret < 0) return ret;
    if (next_source_frame(i, &id, &data) != 0) { /* taken meanwhile: the slot goes back */
      ret = mtl_tx_release(s, u.lease);
      return ret < 0 ? ret : EX_DRAINED_NO_FRAME;
    }
    fill(&u, data);
    u.cookie = id; /* comes back in the result */
    ret = mtl_tx_submit(s, &u);
    if (ret < 0) return ret;
  }
  return EX_DRAINED_NO_FRAME;
}

/* Serves s[i], then arms what it wants next. */
static int serve(mtl_queue_h q, mtl_session_h* s, int i) {
  int d = drain(s[i], i);
  if (d < 0) return d;
  uint64_t want = MTL_WAIT_RESULTS | (d == EX_DRAINED_NO_SLOT ? MTL_WAIT_ACQUIRE : 0);
  int ret = mtl_queue_arm(q, MTL_OBJ_OF_SESSION(s[i]), want, (uint64_t)i);
  return ret > 0 ? 0 : ret; /* 1: a report of s[i] is on its way, with this user */
}

int event_loop(mtl_instance_h mt, mtl_session_h* s, int n) {
  struct mtl_ready r[16];
  struct epoll_event ev = {.events = EPOLLIN}, ready[2];
  intptr_t qfd = -1;
  mtl_queue_h q = MTL_NULL(mtl_queue_h);
  int src = source_eventfd();
  int ep = epoll_create1(0);
  int ret = ep < 0 ? -MTL_ENOMEM : mtl_queue_create(mt, 0, &q, &qfd);
  if (ret >= 0 && (epoll_ctl(ep, EPOLL_CTL_ADD, (int)qfd, &ev) < 0 ||
                   epoll_ctl(ep, EPOLL_CTL_ADD, src, &ev) < 0))
    ret = -MTL_EINVAL;
  for (int i = 0; ret >= 0 && i < n; i++) ret = serve(q, s, i);
  while (ret >= 0 && g_running) {
    int k = mtl_queue_wait(q, r, sizeof(r[0]), 16, 0);
    if (k == -MTL_EAGAIN) {          /* the descriptor is armed: sleep now */
      epoll_wait(ep, ready, 2, 100); /* the 100 ms only poll g_running */
      k = 0;
    }
    if (k < 0) {
      ret = k; /* -MTL_ECANCELED, -MTL_ESHUTDOWN: leave */
      break;
    }
    uint64_t frames; /* read before serving, so a frame queued meanwhile wakes the loop */
    if (read(src, &frames, sizeof(frames)) < 0 && errno != EAGAIN) ret = -MTL_EIO;
    for (int j = 0; ret >= 0 && j < k; j++) ret = serve(q, s, (int)r[j].user);
    for (int i = 0; ret >= 0 && i < n; i++) /* armed for results only, a frame came */
      if (source_frame_waiting(i)) ret = serve(q, s, i);
  }
  if (ep >= 0)
    close(ep); /* the descriptor out of the epoll set before the queue closes */
  mtl_queue_close(q);
  return ret < 0 ? ex_fail("loop", ret) : 0;
}
