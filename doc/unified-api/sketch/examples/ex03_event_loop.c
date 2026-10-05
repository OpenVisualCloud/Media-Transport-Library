/* ex03 — a sender in the application's own epoll loop, reading a result per frame.
   s: a started TX session created with MTL_SESSION_RESULTS in sc.flags. A data call that
   finds nothing returns -MTL_EAGAIN and arms the wait handle, so the loop is "drain until
   -MTL_EAGAIN, then sleep in epoll". Needs: MS1. */
#include <sys/epoll.h>
#include <unistd.h>

#include "ex_common.h"

int next_source_frame(uint64_t* id, const void** data); /* 0 = one taken */
void fill(struct mtl_unit* u, const void* data);
void source_frame_done(uint64_t id, uint32_t status, int64_t margin_ns);

static int drain(mtl_session_h s) {
  struct mtl_tx_result r[16];
  struct mtl_unit u;
  int n;
  MTL_INIT(&u);
  while ((n = mtl_tx_reap(s, r, 16, 0)) > 0) /* in submission order */
    for (int i = 0; i < n; i++)
      source_frame_done(r[i].cookie, r[i].status, r[i].margin_ns);
  if (n != -MTL_EAGAIN) return n;
  for (;;) { /* slot first, then the source frame: none is taken without a place to go */
    uint64_t id;
    const void* data;
    int ret = mtl_tx_acquire(s, &u, 0);
    if (ret < 0) return ret == -MTL_EAGAIN ? 0 : ret;
    if (next_source_frame(&id, &data) != 0) return mtl_tx_release(s, u.lease);
    fill(&u, data);
    u.cookie = id; /* comes back in the result */
    ret = mtl_tx_submit(s, &u);
    if (ret < 0) return ret;
  }
}

int event_loop(mtl_session_h s) {
  intptr_t fd;
  struct epoll_event ev = {.events = EPOLLIN};
  int ep = epoll_create1(0);
  int ret =
      ep < 0 ? -MTL_ENOMEM
             : mtl_session_get_wait_handle(s, MTL_WAIT_ACQUIRE | MTL_WAIT_RESULTS, &fd);
  if (ret >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, (int)fd, &ev) < 0) ret = -MTL_EINVAL;
  while (ret >= 0 && g_running) {
    ret = drain(s);
    if (ret >= 0) epoll_wait(ep, &ev, 1, 100);
  }
  if (ep >= 0) close(ep);
  return ret < 0 ? ex_fail("loop", ret) : 0;
}
