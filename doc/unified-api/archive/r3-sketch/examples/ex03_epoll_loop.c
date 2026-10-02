/* 10 §5 — results in an epoll loop (P2, P7): trywait returns 1 / 0 / < 0. */
#include <sys/epoll.h>
#include <unistd.h>

#include "ex_common.h"

void log_not_on_time(uint64_t cookie, uint32_t status, uint32_t reason,
                     int64_t margin_ns);
void release_source_frame(uint64_t cookie);
int next_source_frame(uint64_t* cookie, void** data); /* 0 = one ready, < 0 = none */
void fill(const struct mtl_buffer_view* v, const void* data);

static int loop(mtl_session_h s, uint64_t mask, int ep) {
  int ret;
  while (g_running) {
    int ready = mtl_session_trywait(s, mask);
    if (ready < 0) return ex_fail("trywait", ready);
    if (ready == 0) { /* armed: safe to block; the library drains the fd */
      struct epoll_event evs[8];
      if (epoll_wait(ep, evs, 8, 100) < 0) continue;
    }

    struct mtl_tx_result r[16];
    int n = mtl_tx_reap(s, r, sizeof(r[0]), 16, 0); /* the stride is an argument (A3) */
    if (n < 0) return ex_fail("reap", n);
    for (int i = 0; i < n; i++) {
      if (r[i].hdr.status != MTL_TX_ON_TIME)
        log_not_on_time(r[i].hdr.user_cookie, r[i].hdr.status, r[i].reason,
                        r[i].timing.margin_ns);
      release_source_frame(r[i].hdr.user_cookie);
    }

    for (;;) { /* feed until back-pressure */
      uint64_t cookie;
      void* data;
      if (next_source_frame(&cookie, &data) < 0) break;
      mtl_lease_h lease;
      struct mtl_buffer_view v;
      mtl_buffer_view_init(&v);
      ret = mtl_tx_acquire(s, &lease, &v, NULL, 0);
      if (ret == -MTL_EAGAIN) break; /* nothing now: back to trywait */
      if (ret < 0) return ex_fail("acquire", ret);
      fill(&v, data);
      struct mtl_tx_submission sub;
      mtl_tx_submission_init(&sub);
      sub.user_cookie = cookie;
      ret = mtl_tx_submit(s, lease, &sub);
      if (ret < 0) {
        mtl_tx_release(s, lease);
        return ex_fail("submit", ret);
      }
    }
  }
  return 0;
}

/* s: a started TX session created with sc.completion.mode = MTL_COMPLETE_ALL */
int run_loop(mtl_session_h s) {
  const uint64_t mask = MTL_WAIT_RESULTS | MTL_WAIT_ACQUIRE;
  struct mtl_wait_object wo;
  int ret = mtl_session_get_wait_object(s, mask, &wo);
  if (ret < 0) return ex_fail("wait object", ret);
  if (wo.kind != MTL_WAIT_FD) return -MTL_ENOTSUP; /* Windows: MTL_WAIT_WIN_HANDLE */

  int ep = epoll_create1(0);
  if (ep < 0) return -MTL_ENOMEM;
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.u64 = s.id;
  if (epoll_ctl(ep, EPOLL_CTL_ADD, (int)wo.native, &ev) < 0)
    ret = -MTL_EINVAL;
  else
    ret = loop(s, mask, ep);
  close(ep);
  return ret;
}
