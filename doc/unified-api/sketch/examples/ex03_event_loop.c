/* ex03 — many senders served by one thread, woken by MTL and by the frame source. Each
   session is created with MTL_SESSION_RESULTS. Needs: MS2 (MS2a: queues). */
#include "ex_common.h"

/* The source: decoder threads queue frames per session; only this thread takes them. */
void source_start(mtl_queue_h q);             /* its threads call frame_queued(q) */
int source_next_ready(void);                  /* a session with new frames, -1: none */
const void* source_peek(int i, uint64_t* id); /* s[i]'s oldest frame, NULL: none */
void source_pop(int i);
void source_frame_done(uint64_t id, uint32_t status);
void fill(struct mtl_unit* u, const void* data);

/* On a decoder thread, after it queued a frame: wakes the loop. */
void frame_queued(mtl_queue_h q) {
  mtl_queue_post(q, 1); /* -MTL_EBADF once the loop closed q */
}

/* Sends the frames waiting for s[i]. 1: a frame waits for a free slot. */
static int send_waiting(mtl_session_h s, int i) {
  const void* frame;
  uint64_t id;
  struct mtl_unit u;
  MTL_INIT(&u);
  while ((frame = source_peek(i, &id)) != NULL) {
    int ret = mtl_tx_acquire(s, &u, 0);
    if (ret == -MTL_EAGAIN) return 1;
    if (ret < 0) return ret;
    fill(&u, frame);
    u.cookie = id; /* comes back in the result */
    source_pop(i);
    ret = mtl_tx_submit(s, &u);
    if (ret < 0) return ret;
  }
  return 0;
}

/* A report disarms the session. Serve it without waiting, then arm what you want next:
   results always, a free slot only while a frame waits for one. */
static int serve(mtl_queue_h q, mtl_session_h s, int i) {
  struct mtl_tx_result r[16];
  int n;
  while ((n = mtl_tx_reap(s, r, 16, 0)) > 0)
    for (int k = 0; k < n; k++) source_frame_done(r[k].cookie, r[k].status);
  if (n != -MTL_EAGAIN) return n;
  int need_slot = send_waiting(s, i);
  if (need_slot < 0) return need_slot;
  uint64_t want = MTL_WAIT_RESULTS | (need_slot ? MTL_WAIT_ACQUIRE : 0);
  int ret = mtl_queue_arm(q, MTL_OBJ_OF_SESSION(s), want, (uint64_t)i);
  return ret < 0 ? ret : 0; /* 1: a report is already on its way */
}

/* Returns when the instance is interrupted (ex_common.h), which ends the queue's wait. */
int event_loop(mtl_instance_h mt, mtl_session_h* s, int n) {
  struct mtl_ready r[16];
  mtl_queue_h q;
  int ret = mtl_queue_create(mt, 0, &q, NULL); /* own loop (GLib, libuv): pass &fd */
  if (ret < 0) return ex_fail("queue", ret);
  source_start(q);
  for (int i = 0; ret >= 0 && i < n; i++) ret = serve(q, s[i], i);
  while (ret >= 0) {
    int k = mtl_queue_wait(q, r, sizeof(r[0]), 16, MTL_FOREVER);
    for (int j = 0; j < k && ret >= 0; j++) {
      int i = (int)r[j].user;
      if (r[j].o.kind == MTL_OBJ_NONE) /* the source posted: serve who got frames */
        while (ret >= 0 && (i = source_next_ready()) >= 0) ret = serve(q, s[i], i);
      else if (r[j].o.kind == MTL_OBJ_SESSION)
        ret = serve(q, s[i], i);
    }
    if (k < 0) ret = k;
  }
  mtl_queue_close(q);
  return ret == -MTL_ECANCELED ? 0 : ex_fail("loop", ret);
}
