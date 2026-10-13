/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"

namespace {

constexpr int kFrames = 8;
constexpr uint16_t kFbCnt = 3;
/* The port St20pHandler's RX listens on. */
constexpr uint16_t kBusyUdpPort = 20000;
constexpr uint16_t kIdleUdpPort = 20002;
constexpr uint64_t kSleepUs = 1000;
/* Each frame is requested this far ahead, so packet 0 waits 80-120 ms for its epoch. */
constexpr uint64_t kLeadNs = 100 * NS_PER_MS;
constexpr uint64_t kFramePeriodNs = 250 * NS_PER_MS;
/* The builder fills the 511-packet TX ring here; the transmitter is still waiting. */
constexpr uint64_t kBusyWindowNs = 10 * NS_PER_MS;
/* Ring full, packet 0 not due: no tasklet on the scheduler has work. */
constexpr uint64_t kIdleWindowStartNs = 20 * NS_PER_MS;
constexpr uint64_t kIdleWindowEndNs = 60 * NS_PER_MS;
/* Ring prefill is ~127 bulks; a sleep after each would allow ~11 in the busy window. */
constexpr uint64_t kBusyWindowMinIterations = 64;
/* One iteration per 200 us, the shortest real sleep (sch_zero_sleep_threshold_us). */
constexpr uint64_t kIdleWindowMaxIterations =
    (kIdleWindowEndNs - kIdleWindowStartNs) / (200 * NS_PER_US);

uint64_t monotonicNs() {
  struct timespec spec;
  clock_gettime(CLOCK_MONOTONIC_RAW, &spec);
  return (uint64_t)spec.tv_sec * NS_PER_S + spec.tv_nsec;
}

/* Written from the scheduler thread, read by the test thread. */
struct TaskletSleepState {
  std::atomic<bool> running{false};
  std::atomic<uint64_t> next_start_ns{0};
  std::atomic<int> frames_started{0};
  std::atomic<int> frames_done{0};
  std::array<std::atomic<bool>, kFbCnt> in_flight{};
  std::array<std::atomic<uint64_t>, kFrames> start_ns{};
  std::array<std::atomic<uint64_t>, kFrames> busy_iterations{};
  std::array<std::atomic<uint64_t>, kFrames> idle_iterations{};
};

int busyNextFrame(void* priv, uint16_t* next_frame_idx, struct st20_tx_frame_meta* meta) {
  auto* st = static_cast<TaskletSleepState*>(priv);
  const uint64_t now = monotonicNs();
  const int frame = st->frames_started.load(std::memory_order_relaxed);

  if (!st->running.load(std::memory_order_acquire) || frame >= kFrames) return -EBUSY;
  if (now < st->next_start_ns.load(std::memory_order_relaxed)) return -EBUSY;
  const uint16_t idx = frame % kFbCnt;
  if (st->in_flight[idx].load(std::memory_order_acquire)) return -EBUSY;

  st->in_flight[idx].store(true, std::memory_order_relaxed);
  st->start_ns[frame].store(now, std::memory_order_relaxed);
  st->next_start_ns.store(now + kFramePeriodNs, std::memory_order_relaxed);
  st->frames_started.store(frame + 1, std::memory_order_release);
  meta->tfmt = ST10_TIMESTAMP_FMT_TAI;
  meta->timestamp = NoCtxTest::FakePtpClockNow(nullptr) + kLeadNs;
  *next_frame_idx = idx;
  return 0;
}

int busyFrameDone(void* priv, uint16_t frame_idx, struct st20_tx_frame_meta* meta) {
  auto* st = static_cast<TaskletSleepState*>(priv);
  (void)meta;
  st->in_flight[frame_idx].store(false, std::memory_order_release);
  st->frames_done.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

/* Called once per scheduler iteration, so it counts them. */
int idleNextFrame(void* priv, uint16_t* next_frame_idx, struct st20_tx_frame_meta* meta) {
  auto* st = static_cast<TaskletSleepState*>(priv);
  (void)next_frame_idx;
  (void)meta;
  const int started = st->frames_started.load(std::memory_order_acquire);
  if (!started) return -EBUSY;
  const int frame = started - 1;
  const uint64_t since =
      monotonicNs() - st->start_ns[frame].load(std::memory_order_relaxed);
  if (since <= kBusyWindowNs)
    st->busy_iterations[frame].fetch_add(1, std::memory_order_relaxed);
  else if (since >= kIdleWindowStartNs && since <= kIdleWindowEndNs)
    st->idle_iterations[frame].fetch_add(1, std::memory_order_relaxed);
  return -EBUSY;
}

struct St20TxFree {
  void operator()(st20_tx_handle handle) const {
    st20_tx_free(handle);
  }
};
using St20TxPtr = std::unique_ptr<st_tx_video_session_handle_impl, St20TxFree>;

St20TxPtr createTx(st_tests_context* ctx, TaskletSleepState* st, const char* name,
                   uint16_t udp_port, uint32_t flags,
                   int (*next_frame)(void*, uint16_t*, struct st20_tx_frame_meta*),
                   int (*frame_done)(void*, uint16_t, struct st20_tx_frame_meta*)) {
  struct st20_tx_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.name = name;
  ops.priv = st;
  ops.num_port = 1;
  memcpy(ops.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops.pacing = ST21_PACING_NARROW;
  ops.type = ST20_TYPE_FRAME_LEVEL;
  ops.packing = ST20_PACKING_BPM;
  ops.width = 1920;
  ops.height = 1080;
  ops.fps = ST_FPS_P25;
  ops.fmt = ST20_FMT_YUV_422_10BIT;
  ops.payload_type = 112;
  ops.flags = flags;
  ops.framebuff_cnt = kFbCnt;
  ops.get_next_frame = next_frame;
  ops.notify_frame_done = frame_done;
  return St20TxPtr(st20_tx_create(ctx->handle, &ops));
}

}  // namespace

/* st20_tx_tasklet_sleep_idle_peer
 * Config:    MTL_FLAG_TASKLET_SLEEP, sleep forced to kSleepUs; CNI and RX video off the
 *            TX scheduler; initDefaultContext(). Two st20 frame-level TX sessions,
 *            1080p25 BPM, on TEST_PORT_1 and one scheduler: the busy one created
 *            first, then an idle one whose get_next_frame is always busy and so runs
 *            once per scheduler iteration. st20p RX of the busy stream on TEST_PORT_2.
 * Plan:      kFrames frames, one every kFramePeriodNs, each USER_PACING kLeadNs ahead.
 *            Packet 0 then waits for its epoch, so the transmitter has nothing to do,
 *            while the builder still has the whole TX ring to fill.
 * Expect:    per frame, scheduler iterations (idle get_next_frame calls):
 *            1. >= kBusyWindowMinIterations within kBusyWindowNs of the frame start:
 *               the scheduler did not sleep while the busy session had packets to build
 *            2. <= kIdleWindowMaxIterations from kIdleWindowStartNs to kIdleWindowEndNs:
 *               it does sleep once nothing has work, so 1. is not vacuous
 *            then: kFrames started, >= kFrames - 1 done (the last may still hold
 *            mbufs), >= kFrames - 1 received.
 * Skip/Fail: FAIL if the two TX sessions land on different schedulers.
 */
TEST_F(NoCtxTest, st20_tx_tasklet_sleep_idle_peer) {
  ctx->para.flags |= MTL_FLAG_TASKLET_SLEEP | MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
  ctx->para.flags &= ~MTL_FLAG_CNI_TASKLET;
  initDefaultContext();
  ASSERT_EQ(mtl_sch_set_sleep_us(ctx->handle, kSleepUs), 0);

  TaskletSleepState st;
  auto rxBundle =
      createSt20pHandlerBundle(/*createTx=*/false, /*createRx=*/true, nullptr);
  St20TxPtr busy = createTx(ctx, &st, "st20_tx_busy", kBusyUdpPort,
                            ST20_TX_FLAG_USER_PACING, busyNextFrame, busyFrameDone);
  ASSERT_NE(busy, nullptr);
  St20TxPtr idle =
      createTx(ctx, &st, "st20_tx_idle", kIdleUdpPort, 0, idleNextFrame, nullptr);
  ASSERT_NE(idle, nullptr);
  ASSERT_EQ(st20_tx_get_sch_idx(busy.get()), st20_tx_get_sch_idx(idle.get()))
      << "the sessions must share one tasklet";

  rxBundle.handler->startSessionRx();
  StartFakePtpClock();
  ASSERT_EQ(mtl_start(ctx->handle), 0);
  st.next_start_ns.store(monotonicNs() + NS_PER_S);
  st.running.store(true, std::memory_order_release);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (st.frames_started.load() < kFrames &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::this_thread::sleep_for(std::chrono::nanoseconds(kFramePeriodNs));
  st.running.store(false);
  rxBundle.handler->stopSession();

  const int started = st.frames_started.load(std::memory_order_acquire);
  ASSERT_EQ(started, kFrames);
  for (int i = 0; i < started; i++) {
    RecordProperty("frame" + std::to_string(i) + "_iterations",
                   std::to_string(st.busy_iterations[i].load()) + "/" +
                       std::to_string(st.idle_iterations[i].load()));
    EXPECT_GE(st.busy_iterations[i].load(), kBusyWindowMinIterations)
        << "frame " << i << ": the scheduler slept while the busy session had work";
    EXPECT_LE(st.idle_iterations[i].load(), kIdleWindowMaxIterations)
        << "frame " << i << ": the scheduler did not sleep with nothing to do";
  }
  EXPECT_GE(st.frames_done.load(), kFrames - 1);
  EXPECT_GE(rxBundle.handler->rxFrames(), (uint32_t)(kFrames - 1));
}
