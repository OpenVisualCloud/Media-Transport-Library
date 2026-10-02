// examples_cpp.cpp — the core API from C++17: MTL_INIT, typed handles, options, an RAII
// lease guard and a stride-safe result read. Bindings follow the same pattern.
#include <mtl/experimental/mtl.h>
#include <mtl/experimental/mtl_options.h>

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

}  // namespace

int cpp_sender(mtl_instance_h mt) {
  mtl_session_config sc;
  MTL_INIT(&sc);
  sc.direction = MTL_TX;
  sc.essence = MTL_VIDEO;
  sc.flags = MTL_SESSION_RESULTS;
  sc.video.raster.width = 1920;
  sc.video.raster.height = 1080;
  sc.video.raster.fps = mtl_fps_rational(MTL_FPS_59_94);
  sc.video.format = MTL_YUV422_10;
  sc.flows[0].ip[0] = 239;
  sc.flows[0].ip[1] = 168;
  sc.flows[0].ip[2] = 85;
  sc.flows[0].ip[3] = 20;
  sc.flows[0].udp_port = 20000;
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
