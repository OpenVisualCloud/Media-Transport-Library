// examples_cpp.cpp — the core API from C++17: MTL_INIT, typed handles, options, an RAII
// lease guard and a stride-safe result read; then a test bench on the null backend: a
// capacity dry run, a start at a TAI instant, a shared queue read by a library thread,
// the full timing record, the test clock and an injected fault. Bindings follow the same
// pattern.
#include <mtl/experimental/mtl.h>
#include <mtl/experimental/mtl_debug.h>
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_options.h>
#include <mtl/experimental/mtl_queue.h>

#include <array>
#include <cstdio>

extern volatile int g_running;

namespace {

// Releases an acquired TX lease unless it was handed to submit (which keeps it only on
// -MTL_EAGAIN).
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
    int ret = mtl_tx_submit(s_, &u_);
    held_ = ret == -MTL_EAGAIN;
    return ret;
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
  sc->flows[0].ip[0] = 239;
  sc->flows[0].ip[1] = 168;
  sc->flows[0].ip[2] = 85;
  sc->flows[0].ip[3] = 20;
  sc->flows[0].udp_port = 20000;
}

// Runs on the queue's library thread, never on a tasklet: every TX result and event of
// the sessions bound to the queue, plus the instance events it subscribed to.
void on_queue(void* user, const mtl_tx_result* r, const mtl_event* ev) {
  (void)user;
  if (r && r->status != MTL_TX_ON_TIME)
    std::printf("unit %llu: %s\n", static_cast<unsigned long long>(r->seq),
                mtl_reason_name(r->reason));
  if (ev)
    std::printf("%s: event %u, %s\n", ev->origin_name, ev->type,
                mtl_reason_name(ev->reason));
}

}  // namespace

int cpp_sender(mtl_instance_h mt) {
  mtl_session_config sc;
  video_tx_config(&sc);
  // a tuning knob: options are absent unless set, and absent means the default
  const std::array<mtl_option, 1> opts{
      {{MTL_OPT_LATE_POLICY, 0, MTL_LATE_SEND_LATE, nullptr}}};
  sc.options = opts.data();
  sc.option_count = static_cast<uint32_t>(opts.size());

  mtl_session_h s = MTL_NULL(mtl_session_h);
  int ret = mtl_session_create(mt, &sc, &s);
  if (ret >= 0) ret = mtl_session_start(&s, 1, nullptr, nullptr);
  while (ret >= 0 && g_running) {
    TxLease lease(s);
    ret = lease.acquire(MTL_MS(100));
    if (ret == -MTL_EAGAIN) {
      ret = 0;
      continue;
    }
    if (ret == 0) ret = lease.submit();
    std::array<mtl_tx_result, 8> r{};
    int n = mtl_tx_reap(s, r.data(), sizeof(r[0]), static_cast<uint32_t>(r.size()), 0);
    for (int i = 0; i < n; i++)
      if (r[i].status != MTL_TX_ON_TIME)
        std::printf("unit %llu: %s\n", static_cast<unsigned long long>(r[i].seq),
                    mtl_reason_name(r[i].reason));
  }
  mtl_session_close(s, MTL_SEC(1));
  return ret;
}

// No NIC, no root, no hugepages: a null port, and a clock that moves only when told
// (mtl_debug.h: a debug build of the library, else -MTL_ENOTSUP).
int cpp_test_bench() {
  mtl_port_spec port{};
  std::snprintf(port.name, sizeof(port.name), "null:1");
  mtl_instance_params p;
  MTL_INIT(&p);
  p.port_count = 1;
  p.ports = &port;
  mtl_instance_h mt = MTL_NULL(mtl_instance_h);
  int ret = mtl_instance_open(&p, &mt);
  if (ret < 0) return ret;
  ret = mtl_test_clock(mt, 0, mtl_rational{0, 0});

  // the dry run of create: grants, and with CHECK_CAPACITY also the free capacity
  mtl_session_config sc;
  video_tx_config(&sc);
  mtl_session_info info{};
  if (ret >= 0)
    ret = mtl_session_query(mt, &sc, MTL_QUERY_CHECK_CAPACITY, &info, sizeof(info),
                            nullptr, 0);
  if (ret == -MTL_ENOSPC) {
    mtl_error_info e{};
    mtl_last_error(&e, sizeof(e));
    std::printf("no room: %s\n", mtl_reason_name(e.reason));  // a CAPACITY_* reason
  }

  // results and events of the session, and the instance's time events, on one queue
  mtl_session_h s = MTL_NULL(mtl_session_h);
  mtl_queue_h q = MTL_NULL(mtl_queue_h);
  mtl_queue_config qc;
  MTL_INIT(&qc);
  qc.subscribe = MTL_SUB_TIME;
  if (ret >= 0) ret = mtl_session_create(mt, &sc, &s);
  if (ret >= 0) ret = mtl_queue_create(mt, &qc, &q);
  if (ret >= 0) ret = mtl_queue_bind(q, s, MTL_BIND_RESULTS | MTL_BIND_EVENTS);

  // start at an instant: 100 ms from now on the instance clock
  int64_t now = 0;
  if (ret >= 0) ret = mtl_time_now(mt, &now);
  mtl_when when{};
  when.kind = MTL_AT_TAI;
  when.value = now + MTL_MS(100);
  if (ret >= 0) ret = mtl_session_start(&s, 1, &when, nullptr);
  for (int i = 0; ret >= 0 && i < 3; i++) {
    TxLease lease(s);
    ret = lease.acquire(0);
    if (ret == 0) ret = lease.submit();
    if (ret >= 0) ret = mtl_test_clock_advance(mt, MTL_MS(50));
  }

  // the full timing record: pass its size; the NIC launch time is valid only when flagged
  std::array<mtl_tx_result_full, 4> full{};
  int n = ret < 0 ? 0
                  : mtl_queue_reap(q, full.data(), sizeof(full[0]),
                                   static_cast<uint32_t>(full.size()), 0);
  for (int i = 0; i < n; i++)
    if (full[i].detail_flags & MTL_TXF_OBSERVED_LEG0)
      std::printf("launch error %lld ns\n",
                  static_cast<long long>(full[i].observed_first_tai_ns[0] -
                                         full[i].scheduled_tai_ns));

  // from here a library thread reads the queue; then the time source is lost on purpose
  if (ret >= 0) ret = mtl_queue_dispatch_start(q, on_queue, nullptr);
  mtl_fault_params f;
  MTL_INIT(&f);
  if (ret >= 0) ret = mtl_debug_inject(MTL_OBJ_OF_INSTANCE(mt), MTL_FAULT_TIME_LOST, &f);
  if (ret >= 0) ret = mtl_test_clock_advance(mt, MTL_SEC(1));
  mtl_queue_dispatch_stop(q);

  mtl_session_close(s, MTL_SEC(1));
  mtl_queue_close(q);
  mtl_instance_close(mt, MTL_SEC(1));
  return ret;
}
