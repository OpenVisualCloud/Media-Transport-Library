// SPDX-License-Identifier: BSD-3-Clause
// C++17 twin of the doc examples: C++ and bindings build every struct with the exported
// *_init() functions (the MTL_*_INIT value macros are C only) and use the handle helpers.
#include <mtl/experimental/mtl_debug.h>
#include <mtl/experimental/mtl_simple.h>
#include <mtl/experimental/mtl_unified.h>

#include <array>
#include <cstdio>
#include <vector>

namespace {

int fail(const char* what, int ret) {
  mtl_error_info e;
  mtl_error_info_init(&e);
  mtl_last_error(&e);
  std::fprintf(stderr, "%s: %s %s\n", what, mtl_error_name(ret), e.detail);
  return ret;
}

void on_entry(void* user, const mtl_cq_entry* e) {
  auto* count = static_cast<uint64_t*>(user);
  if (e->hdr.kind == MTL_CQE_TX_RESULT && e->tx.hdr.status == MTL_TX_ON_TIME) ++*count;
}

}  // namespace

int cpp_tx(mtl_instance_h mt) {
  mtl_session_config sc;
  mtl_session_config_init(&sc);
  sc.direction = MTL_DIR_TX;
  if (int ret = mtl_flow_parse("239.168.85.20:20000", &sc.flows[0]); ret < 0)
    return fail("flow", ret);
  sc.completion.mode = MTL_COMPLETE_ALL;
  sc.flags = MTL_SESSION_SINGLE_READER;
  sc.timing.timeline = mtl_timeline_epoch(mt);

  mtl_video_config vc;
  mtl_video_config_init(&vc);
  vc.width = 1920;
  vc.height = 1080;
  if (int ret = mtl_fps_parse("59.94", &vc.fps); ret < 0) return fail("fps", ret);
  vc.transport_format = MTL_VIDEO_YUV422_10BIT;

  mtl_session_info info;
  mtl_buffer_requirements req;
  mtl_session_info_init(&info);
  mtl_buffer_requirements_init(&req);
  if (int ret =
          mtl_video_session_query(mt, &sc, &vc, MTL_QUERY_CHECK_CAPACITY, &info, &req);
      ret < 0)
    return fail("query", ret);

  mtl_session_h s = MTL_SESSION_NULL;
  if (int ret = mtl_video_session_create(mt, &sc, &vc, &s); ret < 0)
    return fail("create", ret);
  if (mtl_session_is_null(s) || !mtl_session_eq(s, s)) return -MTL_EBADF;

  mtl_start_params sp;
  mtl_start_params_init(&sp);
  sp.mode = MTL_START_AT_TAI;
  mtl_time now;
  if (mtl_time_now(mt, &now) == 0) sp.tai_ns = now.ns + MTL_MS(100);
  int ret = mtl_session_start(s, &sp);

  std::vector<mtl_buffer_view> views(info.pool_count);  // cache-by-index pattern
  for (auto& v : views) mtl_buffer_view_init(&v);

  for (int frame = 0; ret >= 0 && frame < 100; frame++) {
    mtl_lease_h lease = MTL_LEASE_NULL;
    ret = mtl_tx_acquire(s, &lease, nullptr, nullptr, MTL_MS(40));
    if (ret < 0) break;
    uint32_t idx = mtl_buffer_index(mtl_lease_buffer(lease));
    if (idx < views.size() && views[idx].plane_count == 0)
      mtl_buffer_get_view(mtl_lease_buffer(lease), &views[idx]);
    mtl_tx_submission sub;
    mtl_tx_submission_init(&sub);
    sub.user_cookie = static_cast<uint64_t>(frame);
    ret = mtl_tx_submit(s, lease, &sub);
    if (ret < 0) {
      mtl_tx_release(s, lease);
      break;
    }
    std::array<mtl_tx_result, 16> res{};
    int n =
        mtl_tx_reap(s, res.data(), sizeof(res[0]), static_cast<uint32_t>(res.size()), 0);
    for (int i = 0; i < n; i++) {
      int64_t lateness = 0;
      const mtl_tx_result& r = res[static_cast<size_t>(i)];
      if (mtl_time_diff_ns(r.timing.observed_first_tai_ns[0],
                           r.timing.scheduled_first_tai_ns, r.hdr.time_valid,
                           MTL_TT_OBSERVED_FIRST_LEG0 | MTL_TT_SCHEDULED, &lateness) == 0)
        std::printf("launch error %lld ns\n", static_cast<long long>(lateness));
    }
  }

  // shared-CQ style read of the same session through its private CQ, any record kind
  mtl_cq_h cq = MTL_CQ_NULL;
  if (mtl_session_get_cq(s, &cq) == 0) {
    std::vector<mtl_cq_entry> entries(8);
    int n = mtl_cq_read(cq, entries.data(), sizeof(mtl_cq_entry),
                        static_cast<uint32_t>(entries.size()), 0);
    uint64_t on_time = 0;
    for (int i = 0; i < n; i++) on_entry(&on_time, &entries[static_cast<size_t>(i)]);
    void* dispatcher = nullptr;  // L4 helper: the same function on a library thread
    if (mtl_cq_dispatch_start(cq, on_entry, &on_time, &dispatcher) == 0)
      mtl_cq_dispatch_stop(dispatcher);
  }

  mtl_session_stop(s, MTL_STOP_FLUSH, 0);
  return mtl_session_destroy(s, 0);
}

int cpp_debug(void) {
  mtl_instance_h mt = MTL_INSTANCE_NULL;
  if (int ret = mtl_instance_open_simple("null:1", &mt); ret < 0)
    return fail("open", ret);
  mtl_time_test_source(mt, 0, mtl_rational{0, 0});
  mtl_time_test_advance(mt, MTL_SEC(1));
  mtl_fault_params fp;
  mtl_fault_params_init(&fp);
  fp.leg = 0;
  mtl_debug_inject(mtl_object_instance(mt), MTL_FAULT_PTP_LOST, &fp);
  uint32_t known = mtl_struct_known_size(MTL_STRUCT_SESSION_CONFIG);
  if (known < sizeof(mtl_session_config)) std::printf("older library: %u\n", known);
  mtl_simple_h h = MTL_SIMPLE_NULL;
  (void)mtl_simple_is_null(h);
  return mtl_instance_release(mt);
}
