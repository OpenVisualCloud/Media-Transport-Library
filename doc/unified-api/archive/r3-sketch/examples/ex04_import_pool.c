/* 10 §6 — zero-copy TX from a framework pool (P2, P5), and the teardown that waits for
   SESSION_RETIRED before touching buffers, regions or the framework's memory. */
#include "ex_common.h"

#define N 4

/* the framework's pool: one arena, page aligned, N surfaces */
extern void* pool_base;
extern uint64_t pool_len;
int next_framework_frame(uint32_t* surface, uint64_t* frame_id); /* 0 = one ready */
void framework_surface_free(uint64_t frame_id);

static int reap_all(mtl_session_h s, int64_t timeout_ns) {
  struct mtl_tx_result r[8];
  int n = mtl_tx_reap(s, r, sizeof(r[0]), 8, timeout_ns);
  for (int i = 0; i < n; i++) framework_surface_free(r[i].hdr.user_cookie); /* NIC done */
  return n;
}

/* Deferred destroy: wait for SESSION_RETIRED on an app-owned EQ, which outlives the
   session. */
static int destroy_and_wait(mtl_session_h s, mtl_eq_h eq) {
  int ret = mtl_session_destroy(s, 0);
  if (ret < 0) return ex_fail("destroy", ret);
  for (int tries = 0; tries < 50; tries++) {
    struct mtl_event ev;
    int n = mtl_eq_read(eq, &ev, sizeof(ev), 1, MTL_MS(100));
    if (n < 0 && n != -MTL_ETIMEDOUT) return ex_fail("eq read", n);
    if (n == 1 && ev.type == MTL_EVENT_SESSION_RETIRED && ev.origin == s.id) return 0;
  }
  /* escalate a stuck deferred destroy */
  return mtl_session_destroy(s, MTL_DESTROY_FORCE);
}

int zero_copy_tx(mtl_instance_h mt, const struct mtl_video_config* vc) {
  struct mtl_session_config sc;
  mtl_session_config_init(&sc);
  sc.direction = MTL_DIR_TX;
  int ret = mtl_flow_parse("239.168.85.21:20000", &sc.flows[0]);
  if (ret < 0) return ex_fail("flow", ret);
  sc.pool.source = MTL_POOL_ATTACHED; /* completion ALL is forced */
  sc.pool.count = N;
  sc.pool.data_path = MTL_PATH_REQUIRE_DIRECT;

  struct mtl_buffer_requirements req; /* before allocating anything */
  mtl_buffer_requirements_init(&req);
  ret = mtl_video_session_query(mt, &sc, vc, 0, NULL, &req);
  if (ret < 0) return ex_fail("query", ret);
  if (N < req.min_count_direct || pool_len < N * req.plane[0].min_span)
    return -MTL_ENOSPC;

  mtl_eq_h eq; /* app-owned: receives this session's SESSION_* events */
  struct mtl_eq_config ec;
  mtl_eq_config_init(&ec);
  ret = mtl_eq_create(mt, &ec, &eq);
  if (ret < 0) return ex_fail("eq", ret);

  mtl_session_h s;
  ret = mtl_video_session_create(mt, &sc, vc, &s);
  if (ret < 0) {
    ex_fail("create", ret);
    goto out_eq;
  }
  ret = mtl_eq_subscribe(eq, mtl_object_session(s), MTL_EQ_SUB_SESSION);
  if (ret < 0) {
    ex_fail("subscribe", ret);
    goto out_session;
  }

  struct mtl_mem_desc md; /* one region for the whole framework pool */
  mtl_mem_desc_init(&md);
  md.va = pool_base;
  md.length = pool_len;
  md.access = MTL_MEM_READ;        /* TX only reads; attach checks it */
  md.flags = MTL_MEM_MAP_REQUIRED; /* numa 0 = the ports' socket */
  mtl_region_h r;
  ret = mtl_mem_import(mt, &md, &r);
  if (ret < 0) {
    ex_fail("import", ret);
    goto out_session;
  }

  mtl_buffer_h base[N];
  uint32_t created = 0;
  for (; created < N; created++) {
    struct mtl_buffer_desc bd;
    mtl_buffer_desc_init(&bd);
    bd.plane_count = 1;
    bd.user_cookie = created;
    bd.plane[0].region = r;
    bd.plane[0].offset = created * req.plane[0].min_span;
    bd.plane[0].span = req.plane[0].min_span;
    bd.plane[0].row_bytes = req.plane[0].row_bytes;
    bd.plane[0].stride = req.plane[0].stride;
    bd.plane[0].rows = req.plane[0].rows;
    ret = mtl_buffer_create(&bd, &base[created]);
    if (ret < 0) break;
  }
  if (ret >= 0) ret = mtl_session_attach_buffers(s, base, N);
  if (ret >= 0) ret = mtl_session_start(s, NULL);
  if (ret < 0) ex_fail("setup", ret);

  while (ret >= 0 && g_running) {
    uint32_t i;
    uint64_t frame_id;
    if (reap_all(s, 0) < 0) break;
    if (next_framework_frame(&i, &frame_id) < 0) continue;
    mtl_lease_h lease; /* exactly surface i: no copy */
    ret = mtl_tx_acquire_buffer(s, base[i], &lease, NULL, NULL, MTL_MS(20));
    if (ret == -MTL_EBUSY || ret == -MTL_ETIMEDOUT) { /* still in flight */
      ret = 0;
      continue;
    }
    if (ret < 0) break;
    struct mtl_tx_submission sub;
    mtl_tx_submission_init(&sub);
    sub.user_cookie = frame_id;
    ret = mtl_tx_submit(s, lease, &sub);
    if (ret < 0) mtl_tx_release(s, lease);
  }

  /* teardown: results first, then retire, then buffers, then the region, then the memory
   */
  mtl_session_stop(s, MTL_STOP_FLUSH, 0); /* every accepted unit gets a result */
  while (reap_all(s, 0) > 0) {
  }
  ret = destroy_and_wait(s, eq);
  for (uint32_t i = 0; i < created; i++)
    if (mtl_buffer_destroy(base[i]) < 0) ret = -MTL_EBUSY;
  if (mtl_mem_destroy(r) < 0) ret = -MTL_EBUSY; /* until 0: pool_base must stay mapped */
  mtl_eq_destroy(eq);
  return ret;

out_session:
  mtl_session_destroy(s, 0);
out_eq:
  mtl_eq_destroy(eq);
  return ret;
}
