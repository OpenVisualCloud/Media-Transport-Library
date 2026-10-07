// examples_cpp.cpp — the core API from C++17 (MTL_INIT, typed handles, options, an RAII
// lease guard, a typed result read), then a test bench on the null backend, one function
// per topic. Bindings follow the same pattern. Needs: MS5.
#include <mtl/experimental/mtl.h>
#include <mtl/experimental/mtl_debug.h>
#include <mtl/experimental/mtl_events.h>
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_options.h>
#include <mtl/experimental/mtl_sync.h>
#include <mtl/experimental/mtl_util.h>

#include <array>
#include <cstdio>
#include <cstring>

namespace {

// Releases an acquired TX lease unless it was handed to submit (which consumes it, also
// on failure).
class TxLease {
 public:
  explicit TxLease(mtl_session_h s) : s_(s) {
    MTL_INIT(&u_);
  }
  ~TxLease() {
    if (held_) mtl_tx_release(s_, u_.lease);
  }
  TxLease(const TxLease&) = delete;
  TxLease& operator=(const TxLease&) = delete;
  int acquire(int64_t timeout_ns) {
    int ret = mtl_tx_acquire(s_, &u_, timeout_ns);
    held_ = ret == 0;
    return ret;
  }
  int submit() {
    held_ = false;
    return mtl_tx_submit(s_, &u_);
  }
  mtl_unit& unit() {
    return u_;
  }

 private:
  mtl_session_h s_;
  mtl_unit u_;
  bool held_ = false;
};

void video_tx_config(mtl_session_config* sc) {
  MTL_INIT(sc);
  sc->direction = MTL_TX;
  sc->essence = MTL_VIDEO;
  sc->flags = MTL_SESSION_RESULTS;
  sc->video.raster.width = 1920;
  sc->video.raster.height = 1080;
  sc->video.raster.fps = mtl_fps_rational(MTL_FPS_59_94);
  sc->video.format = MTL_YUV422_10;
  mtl_flow_ipv4(&sc->flows[0], 239, 168, 85, 20, 20000);
}

// Moves the test clock (mtl_debug.h) by ns: each advance completes, on this thread, every
// unit that fell due.
int advance(mtl_instance_h mt, int64_t ns) {
  mtl_fault_params f;
  MTL_INIT(&f);
  f.step_ns = ns;
  return mtl_debug_inject(MTL_OBJ_OF_INSTANCE(mt), MTL_FAULT_CLOCK_ADVANCE, &f);
}

// The dry run of create: grants, and with CHECK_CAPACITY also the free capacity.
int check_capacity(mtl_instance_h mt, const mtl_session_config& sc) {
  mtl_session_info info{};
  int ret = mtl_session_query(mt, &sc, MTL_QUERY_CHECK_CAPACITY, &info, sizeof(info),
                              nullptr, 0);
  if (ret == -MTL_ENOSPC) {
    mtl_error_info e{};
    mtl_last_error(&e, sizeof(e));
    std::printf("no room: %s\n", mtl_reason_name(e.reason));  // a CAPACITY_* reason
  }
  return ret;
}

// A start at an instant: 100 ms from now on the instance clock.
int start_in_100ms(mtl_instance_h mt, mtl_session_h s) {
  int64_t now = 0;
  int ret = mtl_time_now(mt, &now, nullptr, nullptr);
  mtl_when when{};
  when.kind = MTL_AT_TAI;
  when.value = now + MTL_MS(100);
  return ret < 0 ? ret : mtl_session_start(&s, 1, &when, nullptr);
}

int run_three_frames(mtl_instance_h mt, mtl_session_h s) {
  int ret = 0;
  for (int i = 0; ret >= 0 && i < 3; i++) {
    TxLease lease(s);
    ret = lease.acquire(0);
    if (ret == 0) ret = lease.submit();
    if (ret >= 0) ret = advance(mt, MTL_MS(50));
  }
  return ret;
}

// The full timing record; the NIC launch time is valid only when flagged.
void print_launches(mtl_session_h s) {
  std::array<mtl_tx_result_full, 4> full{};
  int n = mtl_tx_reap_full(s, full.data(), static_cast<uint32_t>(full.size()), 0);
  for (int i = 0; i < n; i++)
    if (full[i].detail_flags & MTL_TXF_OBSERVED_LEG0)
      std::printf("launch error %lld ns\n",
                  static_cast<long long>(full[i].observed_first_tai_ns[0] -
                                         full[i].scheduled_tai_ns));
}

// The session's packets on every leg to a pcapng file, written by a library worker;
// MTL_EVENT_CAPTURE_DONE on the session when max_pkts are in.
int capture(mtl_session_h s, const char* path) {
  mtl_capture_params p;
  MTL_INIT(&p);
  p.max_pkts = 100000;  // about 23 frames of 1080p
  p.path = path;
  return mtl_session_capture(s, &p);
}

// What a node's PTP daemon (ptp4l, read with pmc) tells MTL when it disciplines the clock
// MTL reads: the grandmaster, and locked = 0 when the node lost it, so readiness and the
// time.* stats follow. clock_class 220 or 228 is an ARB timescale
// (MTL_TIMEF_ARB_TIMESCALE).
int set_time_reference(mtl_instance_h mt, const uint8_t gmid[8], uint8_t clock_class,
                       bool locked) {
  mtl_time_reference ref;
  MTL_INIT(&ref);
  std::memcpy(ref.gmid, gmid, sizeof(ref.gmid));
  ref.clock_class = clock_class;
  ref.locked = locked ? 1u : 0u;
  return mtl_time_set_reference(mt, 0, &ref);  // port 0: every port
}

// The instance's pending events (ports, time, schedulers, health), without waiting; the
// same loop reads a session's with mtl_session_read_events.
void print_instance_events(mtl_instance_h mt) {
  std::array<mtl_event, 8> ev{};
  int n;
  while ((n = mtl_instance_read_events(mt, ev.data(), static_cast<uint32_t>(ev.size()),
                                       0)) > 0)
    for (int i = 0; i < n; i++)
      std::printf("%s: event %u, %s\n", ev[i].origin_name, ev[i].type,
                  mtl_reason_name(ev[i].reason));
}

// The time source is lost on purpose: the instance posts MTL_EVENT_TIME_STATE.
int inject_time_loss(mtl_instance_h mt) {
  mtl_fault_params f;
  MTL_INIT(&f);
  int ret = mtl_debug_inject(MTL_OBJ_OF_INSTANCE(mt), MTL_FAULT_TIME_LOST, &f);
  if (ret >= 0) ret = advance(mt, MTL_SEC(1));
  if (ret >= 0) print_instance_events(mt);
  return ret;
}

void print_result(void* priv, const mtl_tx_result* r) {
  (void)priv;
  if (r->status != MTL_TX_ON_TIME)
    std::printf("unit %llu: %s\n", static_cast<unsigned long long>(r->seq),
                mtl_reason_name(r->reason));
}

}  // namespace

int cpp_sender(mtl_instance_h mt) {
  mtl_session_config sc;
  video_tx_config(&sc);
  // a tuning knob: options are absent unless set, and absent means the default
  mtl_option late{};
  late.key = MTL_OPT_LATE_POLICY;
  late.value = MTL_LATE_DROP;
  sc.options = &late;
  sc.option_count = 1;

  mtl_session_h s = MTL_NULL(mtl_session_h);
  int ret = mtl_session_open(mt, &sc, &s);
  while (ret >= 0) {
    TxLease lease(s);
    ret = mtl_tx_reap_each(s, print_result,
                           nullptr);  // then acquire never waits on results
    if (ret >= 0) ret = lease.acquire(MTL_FOREVER);  // -MTL_ECANCELED: the interrupt
    if (ret == 0) ret = lease.submit();
  }
  mtl_session_close(s, MTL_SEC(1));
  return ret == -MTL_ECANCELED ? 0 : ret;
}

// No NIC, no root, no hugepages: a null port, and a clock that moves only when told
// (mtl_debug.h: a debug build of the library, else -MTL_ENOTSUP).
int cpp_test_bench(const uint8_t gmid[8]) {
  mtl_port_spec port{};
  std::snprintf(port.name, sizeof(port.name), "null:1");
  mtl_instance_params p;
  MTL_INIT(&p);
  p.port_count = 1;
  p.ports = &port;
  mtl_instance_h mt = MTL_NULL(mtl_instance_h);
  int ret = mtl_instance_open(&p, &mt);
  if (ret < 0) return ret;
  mtl_fault_params clock;  // rate {0, 0}: the clock moves only by CLOCK_ADVANCE
  MTL_INIT(&clock);
  ret = mtl_debug_inject(MTL_OBJ_OF_INSTANCE(mt), MTL_FAULT_TEST_CLOCK, &clock);
  if (ret >= 0) ret = set_time_reference(mt, gmid, 6, true);

  mtl_session_config sc;
  video_tx_config(&sc);
  mtl_session_h s = MTL_NULL(mtl_session_h);
  if (ret >= 0) ret = check_capacity(mt, sc);
  if (ret >= 0) ret = mtl_session_create(mt, &sc, &s);
  if (ret >= 0) ret = start_in_100ms(mt, s);
  if (ret >= 0) ret = capture(s, "/tmp/bench.pcapng");
  if (ret >= 0) ret = run_three_frames(mt, s);
  if (ret >= 0) print_launches(s);
  if (ret >= 0) ret = inject_time_loss(mt);

  mtl_session_close(s, MTL_SEC(1));
  mtl_instance_close(mt, MTL_SEC(1));
  return ret;
}
