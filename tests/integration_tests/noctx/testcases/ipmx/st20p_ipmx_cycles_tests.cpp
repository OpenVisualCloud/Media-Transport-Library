/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/* IPMX session teardown gives back all it took: 30 create / stream / free cycles of an
 * st20p pair leave the DPDK heap where the first cycle left it. The instance runs
 * without MTL_FLAG_RANDOM_SRC_PORT, so the cycles repeat one UDP tuple and the CNI
 * UDP stream detect list, which keeps one entry per tuple until mtl_uninit(), stops
 * growing after cycle 1. Catalogue: README.md here.
 *
 * Common to every test here:
 * Config:    initDefaultContext() without MTL_FLAG_RANDOM_SRC_PORT; per cycle one
 *            session TX TEST_PORT_1 -> RX TEST_PORT_2, 1080p60 RGB 4:4:4 8-bit, BPM,
 *            3 buffers, UDP 20000; IPMX cases TX ST20P_TX_FLAG_IPMX, RX
 *            ST20P_RX_FLAG_IPMX.
 * Plan:      per cycle: create RX then TX, mtl_start(), 2 s (about 120 frames),
 *            mtl_stop(), free both.
 * Expect:    per cycle: 1. RX got >= half the frames of 2 s at 60 fps, each stamped by TX
 * with one counter at its first and last word, both equal after cycle n >= 1: 2. DPDK
 * heap live allocations and bytes, all sockets,
 *            <= after cycle 0
 * Log:       heap allocations and bytes after each cycle; IPMX_TEST_HEAP_DUMP=1 adds
 *            rte_memzone_dump() and rte_malloc_dump_heaps() after the first and the
 *            last cycle, to diff.
 */

#ifdef MTL_HAS_IPMX

#include <dlfcn.h>
#include <rte_malloc.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"

namespace {
constexpr int kCycles = 30;
constexpr int kCycleSeconds = 2;
/* half the frames of a cycle at 60 fps, start-up and teardown cost the rest */
constexpr uint32_t kMinCycleFrames = kCycleSeconds * 60 / 2;

/* TX stamps a counter at the first and last word of each frame, RX checks they match */
class St20pFrameStamp : public FrameTestStrategy {
 public:
  explicit St20pFrameStamp(St20pHandler* parentHandler)
      : FrameTestStrategy(parentHandler, true, true) {
  }

  void txTestFrameModifier(void* frame, size_t frame_size) override {
    auto* f = static_cast<st_frame*>(frame);
    auto* p = static_cast<uint8_t*>(f->addr[0]);
    uint32_t stamp = idx_tx++;

    memcpy(p, &stamp, sizeof(stamp));
    memcpy(p + frame_size - sizeof(stamp), &stamp, sizeof(stamp));
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) override {
    auto* f = static_cast<st_frame*>(frame);
    auto* p = static_cast<uint8_t*>(f->addr[0]);
    uint32_t first, last;

    memcpy(&first, p, sizeof(first));
    memcpy(&last, p + frame_size - sizeof(last), sizeof(last));
    if (first != last) mismatches++;
    idx_rx++;
  }

  uint32_t mismatches = 0;
};

/* the tests link only libmtl, so the function is looked up in the process, where
 * libmtl loaded DPDK */
bool heapUsage(uint64_t* allocs, uint64_t* bytes) {
  using StatsFn = int (*)(int socket, struct rte_malloc_socket_stats* stats);
  auto fn = reinterpret_cast<StatsFn>(dlsym(RTLD_DEFAULT, "rte_malloc_get_socket_stats"));

  if (!fn) return false;
  *allocs = 0;
  *bytes = 0;
  for (int socket = 0; socket < RTE_MAX_NUMA_NODES; socket++) {
    struct rte_malloc_socket_stats stats;
    if (fn(socket, &stats) < 0) continue;
    *allocs += stats.alloc_count;
    *bytes += stats.heap_allocsz_bytes;
  }
  return true;
}

void heapDump(const char* when) {
  using DumpFn = void (*)(FILE* f);

  if (!getenv("IPMX_TEST_HEAP_DUMP")) return;
  (void)fprintf(stderr, "==== IPMX heap dump %s ====\n", when);
  for (const char* sym : {"rte_memzone_dump", "rte_malloc_dump_heaps"}) {
    auto fn = reinterpret_cast<DumpFn>(dlsym(RTLD_DEFAULT, sym));
    if (fn) fn(stderr);
  }
  (void)fprintf(stderr, "==== IPMX heap dump %s end ====\n", when);
}

void runCreateFreeCycles(NoCtxTest* test, st_tests_context* ctx, bool ipmx) {
  uint64_t base_allocs = 0, base_bytes = 0, allocs = 0, bytes = 0;

  if (ctx->para.num_ports < 2) {
    throw std::runtime_error("st20p create free cycles test ctx needs at least 2 ports");
  }

  for (int cycle = 0; cycle < kCycles; cycle++) {
    auto bundle = test->createSt20pHandlerBundle(
        /*createTx=*/true, /*createRx=*/true,
        [](St20pHandler* handler) { return new St20pFrameStamp(handler); },
        [=](St20pHandler* handler) {
          handler->fillSt20Ops(20000, 3, ST20_FMT_RGB_8BIT, 1920, 1080, 112, ST_FPS_P60);
          handler->sessionsOpsTx.input_fmt = ST_FRAME_FMT_RGB8;
          handler->sessionsOpsTx.transport_fmt = ST20_FMT_RGB_8BIT;
          handler->sessionsOpsRx.output_fmt = ST_FRAME_FMT_RGB8;
          handler->sessionsOpsRx.transport_fmt = ST20_FMT_RGB_8BIT;
          if (ipmx) {
            handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_IPMX;
            handler->sessionsOpsRx.flags |= ST20P_RX_FLAG_IPMX;
          }
        });
    ASSERT_NE(bundle.handler->sessionsHandleTx, nullptr);
    ASSERT_NE(bundle.handler->sessionsHandleRx, nullptr);
    auto* stamp = static_cast<St20pFrameStamp*>(bundle.strategy);

    bundle.handler->startSession();
    ASSERT_EQ(mtl_start(ctx->handle), 0);
    test->sleepUntilFailure(kCycleSeconds);
    bundle.handler->stopSession();
    ASSERT_EQ(mtl_stop(ctx->handle), 0);
    EXPECT_GE(stamp->idx_rx, kMinCycleFrames) << "cycle " << cycle;
    EXPECT_EQ(stamp->mismatches, 0u) << "cycle " << cycle;
    /* the handler frees its sessions, then its strategy goes */
    test->st20pHandlers.clear();
    test->frameTestStrategies.clear();
    if (::testing::Test::HasFailure()) return;

    ASSERT_TRUE(heapUsage(&allocs, &bytes)) << "rte_malloc_get_socket_stats not found";
    (void)fprintf(stderr,
                  "NoCtx ipmx cycles(%d): heap allocs %" PRIu64 " bytes %" PRIu64 "\n",
                  cycle, allocs, bytes);
    if (cycle == 0) {
      base_allocs = allocs;
      base_bytes = bytes;
      heapDump("after the first cycle");
    }
  }
  heapDump("after the last cycle");
  EXPECT_LE(allocs, base_allocs);
  EXPECT_LE(bytes, base_bytes);
}

void initFixedSourcePortContext(NoCtxTest* test, st_tests_context* ctx) {
  ctx->para.flags &= ~MTL_FLAG_RANDOM_SRC_PORT;
  test->initDefaultContext();
}
}  // namespace

/* st20p_ipmx_create_free_cycles
 * The IPMX pair: each cycle also creates and frees the Sender Report state and pool. */
TEST_F(NoCtxTest, st20p_ipmx_create_free_cycles) {
  initFixedSourcePortContext(this, ctx);
  if (HasFatalFailure()) return;
  runCreateFreeCycles(this, ctx, true);
}

/* st20p_noipmx_create_free_cycles
 * The same pair without the IPMX flags, the baseline. */
TEST_F(NoCtxTest, st20p_noipmx_create_free_cycles) {
  initFixedSourcePortContext(this, ctx);
  if (HasFatalFailure()) return;
  runCreateFreeCycles(this, ctx, false);
}

#endif
