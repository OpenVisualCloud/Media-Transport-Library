/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "harness/mock_plugin.hpp"
#include "log.h"
#include "test_harness.hpp"
#include "tests.hpp"

#define ST20P_TEST_PAYLOAD_TYPE (112)
#define ST20P_TEST_UDP_PORT (20000)
#define EPOCH_DROP_TEST_UDP_PORT_ST20P (20100)

namespace {

using ScopedSt20pTx = mtl_test::ScopedHandle<st20p_tx_handle, st20p_tx_free>;
using ScopedSt20pRx = mtl_test::ScopedHandle<st20p_rx_handle, st20p_rx_free>;
using ScopedStFrame = mtl_test::ScopedHandle<struct st_frame*, st_frame_free>;

inline void st20p_set_handle(tests_context* s, void* h) {
  __atomic_store_n(&s->handle, h, __ATOMIC_RELEASE);
}

inline void* st20p_get_handle(const tests_context* s) {
  return __atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);
}

/* Extended test context owning atomic, dynamically sized external frame slots. */
struct St20pContext : public tests_context {
  mtl_test::ScopedExtFrames ext_frames;
};

struct St20pContextDeleter {
  void operator()(St20pContext* c) const {
    if (!c) return;
    tests_context_unit(c);
    delete c;
  }
};

using ScopedSt20pContext = std::unique_ptr<St20pContext, St20pContextDeleter>;

ScopedSt20pContext make_st20p_context(struct st_tests_context* global_ctx, int idx = 0,
                                      uint16_t fb_cnt = 3, bool check_sha = false) {
  ScopedSt20pContext c(new St20pContext());
  c->ctx = global_ctx;
  c->idx = idx;
  c->fb_cnt = fb_cnt;
  c->fb_idx = 0;
  c->check_sha = check_sha;
  return c;
}

struct ConverterTraits {
  using SessionHandle = st20p_convert_session;
  using PrivHandle = st20_convert_priv;
  using CreateReq = struct st20_converter_create_req;
  using FrameMeta = struct st20_convert_frame_meta;

  static bool use_block_get(struct st_tests_context* /*ctx*/) {
    return false;
  }

  static void on_create_req(struct st_tests_context* /*ctx*/, CreateReq* req,
                            int& sleep_time_us) {
    double fps = st_frame_rate(req->fps);
    if (!fps) fps = 60;
    sleep_time_us = static_cast<int>(1000 * 1000 / fps / 2);
    dbg("%s, sleep_time_us %d\n", __func__, sleep_time_us);
  }

  static void set_block_timeout(SessionHandle /*s*/, uint64_t /*ns*/) {
  }

  static void wake_block(SessionHandle /*s*/) {
  }

  static FrameMeta* get_frame(SessionHandle s) {
    return st20_converter_get_frame(s);
  }

  static int put_frame(SessionHandle s, FrameMeta* f, int result) {
    return st20_converter_put_frame(s, f, result);
  }

  static int process_frame(const CreateReq& req, FrameMeta* frame, int sleep_time_us,
                           int /*rand_ratio*/, int& frame_cnt) {
    if (frame->src->width != req.width || frame->dst->width != req.width ||
        frame->src->height != req.height || frame->dst->height != req.height ||
        frame->src->fmt != req.input_fmt || frame->dst->fmt != req.output_fmt) {
      return -EIO;
    }

    /* copy src sha to the start or end of convert frame */
    if (req.input_fmt == ST_FRAME_FMT_YUV422PLANAR10LE)
      memcpy(frame->dst->addr[0],
             static_cast<uint8_t*>(frame->src->addr[0]) + frame->src->data_size -
                 SHA256_DIGEST_LENGTH,
             SHA256_DIGEST_LENGTH);
    else
      memcpy(static_cast<uint8_t*>(frame->dst->addr[0]) + frame->dst->data_size -
                 SHA256_DIGEST_LENGTH,
             frame->src->addr[0], SHA256_DIGEST_LENGTH);
    st_usleep(static_cast<unsigned int>(sleep_time_us));

    frame_cnt++;
    return 0;
  }
};

using MockConverterPool = mtl_test::MockPluginPool<ConverterTraits>;

struct St20pStreamSpec {
  enum st_fps fps = ST_FPS_P59_94;
  int width = 1920;
  int height = 1080;
  enum st_frame_fmt tx_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  enum st20_fmt t_fmt = ST20_FMT_YUV_422_10BIT;
  enum st_frame_fmt rx_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
};

St20pStreamSpec make_stream(enum st_fps fps, int width, int height, enum st_frame_fmt fmt,
                            enum st20_fmt t_fmt = ST20_FMT_YUV_422_10BIT) {
  return {fps, width, height, fmt, t_fmt, fmt};
}

struct St20pDigestScenario {
  std::vector<St20pStreamSpec> streams;
  enum st_plugin_device device = ST_PLUGIN_DEVICE_TEST;
  int fail_interval = 0;
  int timeout_interval = 0;
  int timeout_ms = 0;
  bool tx_ext = false;
  bool rx_ext = false;
  bool rx_dedicated_ext = false;
  bool check_fps = true;
  bool user_timestamp = false;
  bool vsync = true;
  bool pkt_convert = false;
  size_t line_padding_size = 0;
  bool interlace = false;
  bool user_meta = false;
  bool rtcp = false;
  enum st20_packing packing = ST20_PACKING_BPM;
  enum st21_pacing pacing = ST21_PACING_NARROW;
  uint32_t ssrc = 0;
  bool block_get = false;
  bool rx_timing_parser = false;
  bool rx_auto_detect = false;
  bool zero_payload_type = false;
};

/* Holds the first completed slot past notify_frame_done to verify MANUAL_RELEASE. */
struct tx_ext_release_ctx {
  std::atomic<st20p_tx_handle> handle{nullptr};
  std::atomic<struct st_frame*> held_frame{nullptr};
};

}  // namespace

int st_test_convert_plugin_unregister(struct st_tests_context* ctx) {
  if (ctx->converter_dev_handle) {
    st20_converter_unregister(ctx->converter_dev_handle);
    ctx->converter_dev_handle = NULL;
  }
  return 0;
}

int st_test_convert_plugin_register(struct st_tests_context* ctx) {
  auto st = ctx->handle;
  struct st20_converter_dev c_dev = {};
  c_dev.name = "test_converter";
  c_dev.priv = ctx;
  c_dev.target_device = ST_PLUGIN_DEVICE_TEST;
  c_dev.input_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422RFC4175PG2BE10;
  c_dev.output_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422RFC4175PG2BE10;
  c_dev.create_session = MockConverterPool::create_session;
  c_dev.free_session = MockConverterPool::free_session;
  c_dev.notify_frame_available = MockConverterPool::notify_frame_available;
  ctx->converter_dev_handle = st20_converter_register(st, &c_dev);
  if (!ctx->converter_dev_handle) {
    err("%s, converter register fail\n", __func__);
    return -EIO;
  }

  info("%s, succ\n", __func__);
  return 0;
}

static void plugin_register_test(const char* so_name, bool expect_succ) {
  auto st = st_test_ctx()->handle;

  int pre_nb = st_get_plugins_nb(st);
  int ret = st_plugin_register(st, so_name);
  int new_nb = st_get_plugins_nb(st);

  EXPECT_EQ(ret >= 0, expect_succ);
  if (ret < 0) {
    EXPECT_EQ(pre_nb, new_nb);
  } else {
    EXPECT_EQ(pre_nb + 1, new_nb);
    EXPECT_GE(st_plugin_unregister(st, so_name), 0);
  }
}

/* Via KahawaiTest's DT_RPATH (build.sh prefix) or the loader's search (/usr/local) */
TEST(St20p, plugin_register_single) {
  plugin_register_test("libst_plugin_convert_sample.so", true);
}
TEST(St20p, plugin_register_fail) {
  plugin_register_test("/usr/local/lib/x86_64-linux-gnu/libst_plugin_sample_fail.so",
                       false);
}

static int test_st20p_frame_available(void* priv) {
  static_cast<tests_context*>(priv)->cv.notify_all();
  return 0;
}

static int test_st20p_tx_frame_done(void* priv, struct st_frame* frame) {
  auto* s = static_cast<St20pContext*>(priv);
  auto handle = static_cast<st20p_tx_handle>(st20p_get_handle(s));
  if (!handle) return -EIO;

  __atomic_fetch_add(&s->fb_send_done, 1, __ATOMIC_RELAXED);
  if (!(frame->flags & ST_FRAME_FLAG_EXT_BUF)) return 0;

  const size_t n = s->ext_frames.count();
  for (size_t i = 0; i < n; ++i) {
    if (frame->addr[0] == s->ext_frames.frames()[i].addr[0]) {
      s->ext_frames.release_slot(i);
      dbg("%s(%zu), frame done at %d\n", __func__, i, s->idx);
      st20p_tx_notify_ext_frame_free(handle, frame);
      return 0;
    }
  }

  err("%s(%d), unknown frame_addr %p\n", __func__, s->idx, frame->addr[0]);
  st20p_tx_notify_ext_frame_free(handle, frame);
  return 0;
}

static int test_tx_ext_release_frame_done(void* priv, struct st_frame* frame) {
  auto* s = static_cast<tx_ext_release_ctx*>(priv);
  auto handle = s->handle.load(std::memory_order_acquire);
  if (!handle) return -EIO;
  struct st_frame* expected = nullptr;
  if (s->held_frame.compare_exchange_strong(expected, frame)) return 0;

  st20p_tx_notify_ext_frame_free(handle, frame);
  return 0;
}

static void st20p_tx_ops_init(tests_context* st20, struct st20p_tx_ops* ops_tx) {
  auto ctx = st20->ctx;

  memset(ops_tx, 0, sizeof(*ops_tx));
  ops_tx->name = "st20p_test";
  ops_tx->priv = st20;
  ops_tx->port.num_port = 1;
  memcpy(ops_tx->port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx->port.udp_port[MTL_SESSION_PORT_P] = ST20P_TEST_UDP_PORT + st20->idx;
  ops_tx->port.payload_type = ST20P_TEST_PAYLOAD_TYPE;
  ops_tx->width = 1920;
  ops_tx->height = 1080;
  ops_tx->fps = ST_FPS_P59_94;
  ops_tx->input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  ops_tx->transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops_tx->device = ST_PLUGIN_DEVICE_TEST;
  ops_tx->framebuff_cnt = st20->fb_cnt;
  ops_tx->notify_frame_available = test_st20p_frame_available;
  ops_tx->notify_event = test_ctx_notify_event;
  st20->frame_size =
      st_frame_size(ops_tx->input_fmt, ops_tx->width, ops_tx->height, ops_tx->interlaced);
}

static void st20p_rx_ops_init(tests_context* st20, struct st20p_rx_ops* ops_rx) {
  auto ctx = st20->ctx;

  memset(ops_rx, 0, sizeof(*ops_rx));
  ops_rx->name = "st20p_test";
  ops_rx->priv = st20;
  ops_rx->port.num_port = 1;
  memcpy(ops_rx->port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_rx->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops_rx->port.udp_port[MTL_SESSION_PORT_P] = ST20P_TEST_UDP_PORT + st20->idx;
  ops_rx->port.payload_type = ST20P_TEST_PAYLOAD_TYPE;
  ops_rx->width = 1920;
  ops_rx->height = 1080;
  ops_rx->fps = ST_FPS_P59_94;
  ops_rx->transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops_rx->output_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  ops_rx->device = ST_PLUGIN_DEVICE_TEST;
  ops_rx->framebuff_cnt = st20->fb_cnt;
  ops_rx->notify_frame_available = test_st20p_frame_available;
  ops_rx->notify_event = test_ctx_notify_event;
  st20->frame_size = st_frame_size(ops_rx->output_fmt, ops_rx->width, ops_rx->height,
                                   ops_rx->interlaced);
}

static void st20p_tx_assert_cnt(int expect_st20_tx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st20_tx_sessions_cnt, expect_st20_tx_cnt);
}

static void st20p_rx_assert_cnt(int expect_st20_rx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st20_rx_sessions_cnt, expect_st20_rx_cnt);
}

TEST(St20p, tx_create_free_mix) {
  mtl_test::verify_create_free<struct st20p_tx_ops, st20p_tx_handle, st20p_tx_create,
                               st20p_tx_free>(st20p_tx_ops_init, st20p_tx_assert_cnt, 2,
                                              3, 4);
}
TEST(St20p, rx_create_free_mix) {
  mtl_test::verify_create_free<struct st20p_rx_ops, st20p_rx_handle, st20p_rx_create,
                               st20p_rx_free>(st20p_rx_ops_init, st20p_rx_assert_cnt, 2,
                                              3, 4);
}
TEST(St20p, tx_create_free_max) {
  mtl_test::verify_create_free_max<struct st20p_tx_ops, st20p_tx_handle, st20p_tx_create,
                                   st20p_tx_free>(st20p_tx_ops_init, st20p_tx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}
TEST(St20p, rx_create_free_max) {
  mtl_test::verify_create_free_max<struct st20p_rx_ops, st20p_rx_handle, st20p_rx_create,
                                   st20p_rx_free>(st20p_rx_ops_init, st20p_rx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}
TEST(St20p, tx_create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st20p_tx_ops, st20p_tx_handle, st20p_tx_create,
                                 st20p_tx_free>(st20p_tx_ops_init);
}
TEST(St20p, rx_create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st20p_rx_ops, st20p_rx_handle, st20p_rx_create,
                                 st20p_rx_free>(st20p_rx_ops_init);
}
TEST(St20p, tx_create_expect_fail_fb_cnt) {
  mtl_test::verify_invalid_fb_cnt<struct st20p_tx_ops, st20p_tx_handle, st20p_tx_create,
                                  st20p_tx_free>(st20p_tx_ops_init, 1);
  mtl_test::verify_invalid_fb_cnt<struct st20p_tx_ops, st20p_tx_handle, st20p_tx_create,
                                  st20p_tx_free>(st20p_tx_ops_init,
                                                 ST20_FB_MAX_COUNT + 1);
}
TEST(St20p, rx_create_expect_fail_fb_cnt) {
  mtl_test::verify_invalid_fb_cnt<struct st20p_rx_ops, st20p_rx_handle, st20p_rx_create,
                                  st20p_rx_free>(st20p_rx_ops_init, 1);
  mtl_test::verify_invalid_fb_cnt<struct st20p_rx_ops, st20p_rx_handle, st20p_rx_create,
                                  st20p_rx_free>(st20p_rx_ops_init,
                                                 ST20_FB_MAX_COUNT + 1);
}

static void test_st20p_tx_frame_thread(St20pContext* s) {
  auto handle = static_cast<st20p_tx_handle>(st20p_get_handle(s));
  struct test_user_meta meta;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    struct st_frame* frame = st20p_tx_get_frame(handle);
    if (!frame) {
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait_for(lck, std::chrono::milliseconds(10));
        lck.unlock();
      }
      continue;
    }
    if (frame->data_size != s->frame_size || frame->buffer_size != s->frame_size ||
        frame->width != s->width || frame->height != s->height || frame->fmt != s->fmt) {
      s->incomplete_frame_cnt++;
    }

    const int sent = mtl_test::tx_frames(s);
    if (s->user_timestamp) {
      frame->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
      frame->timestamp = sent;
      dbg("%s(%d), timestamp %d\n", __func__, s->idx, sent);
    }
    if (s->user_meta) {
      meta.magic = TEST_USER_META_MAGIC;
      meta.session_idx = s->idx;
      meta.frame_idx = sent;
      frame->user_meta = &meta;
      frame->user_meta_size = sizeof(meta);
    }
    if (s->ext_frames.count() > 0) {
      const size_t n = s->ext_frames.count();
      size_t slot = static_cast<size_t>(s->ext_idx) % n;
      bool acquired = false;
      while (!s->stop) {
        for (size_t attempt = 0; attempt < n; attempt++) {
          const size_t cand = (slot + attempt) % n;
          if (s->ext_frames.try_acquire_slot(cand)) {
            slot = cand;
            acquired = true;
            break;
          }
        }
        if (acquired) break;
        std::this_thread::yield();
      }
      if (!acquired) {
        st20p_tx_put_frame_abort(handle, frame);
        break;
      }
      int ret = st20p_tx_put_ext_frame(handle, frame, &s->ext_frames.frames()[slot]);
      if (ret < 0) {
        st20p_tx_put_frame_abort(handle, frame);
        s->ext_frames.release_slot(slot);
        err("%s, put ext framebuffer fail %d fb_idx %zu\n", __func__, ret, slot);
        continue;
      }
      s->ext_idx = static_cast<int>((slot + 1) % n);
    } else {
      st20p_tx_put_frame(handle, frame);
    }
    __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
    if (!s->start_time) {
      s->start_time = st_test_get_monotonic_time();
      dbg("%s(%d), start_time %" PRIu64 "\n", __func__, s->idx, s->start_time);
    }
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void test_st20p_rx_user_meta(tests_context* s, struct st_frame* frame) {
  auto* meta = static_cast<const struct test_user_meta*>(frame->user_meta);
  if (!meta) {
    s->user_meta_fail_cnt++;
    return;
  }

  dbg("%s(%d), meta idx session %d frame %d magic 0x%x\n", __func__, s->idx,
      meta->session_idx, meta->frame_idx, meta->magic);
  if (frame->user_meta_size != sizeof(*meta)) s->user_meta_fail_cnt++;
  if (meta->magic != TEST_USER_META_MAGIC) s->user_meta_fail_cnt++;
  if (meta->session_idx != s->idx) s->user_meta_fail_cnt++;
  if (meta->frame_idx <= s->last_user_meta_frame_idx) {
    err("%s(%d), err user meta frame idx %d:%d\n", __func__, s->idx, meta->frame_idx,
        s->last_user_meta_frame_idx);
    s->user_meta_fail_cnt++;
  }
  s->last_user_meta_frame_idx = meta->frame_idx;
}

/* MTL's own ST 2110-21 verdict on the received stream. Only the field-parity cause is
 * asserted on -- the narrow/wide thresholds are load- and host-sensitive -- and other
 * causes are logged. Matching failed_cause by string means a change to that text weakens
 * this rather than breaking it; the deterministic guard is
 * tests/unit/session/st20/interlaced_tp_phase_test.cpp. */
static void test_st20p_rx_check_tp(tests_context* s, struct st_frame* frame) {
  struct st20_rx_tp_meta* tp = frame->tp[MTL_SESSION_PORT_P];

  if (!tp) {
    s->incomplete_frame_cnt++;
    return;
  }
  if (tp->compliant != ST_RX_TP_COMPLIANT_FAILED) return;
  if (strcmp(tp->failed_cause, "field parity off frame grid")) {
    if (!s->tp_other_fail_logged) {
      err("%s(%d), timing parser not compliant, cause %s\n", __func__, s->idx,
          tp->failed_cause);
      s->tp_other_fail_logged = true;
    }
    return;
  }
  if (!s->tp_fail_cnt)
    err("%s(%d), timing parser field parity off frame grid\n", __func__, s->idx);
  s->tp_fail_cnt++;
}

/* ST 2110-21 6.2 frame grid: a first field starts on an even field slot, a second on an
 * odd one. Uses 64-bit TAI first-packet time rather than wrapping 32-bit rtp_timestamp.
 */
static void test_st20p_rx_check_field_phase(tests_context* s, struct st_frame* frame) {
  if (!frame->receive_timestamp) {
    if (!s->field_phase_unverified_cnt)
      err("%s(%d), no receive_timestamp, field phase unverifiable\n", __func__, s->idx);
    s->field_phase_unverified_cnt++;
    return;
  }

  uint64_t slot = static_cast<uint64_t>(static_cast<double>(frame->receive_timestamp) /
                                        s->field_period_ns);
  if (((slot & 0x1) != 0) == frame->second_field) return;
  if (!s->field_phase_fail_cnt)
    err("%s(%d), field phase mismatch, slot %" PRIu64 " second_field %d\n", __func__,
        s->idx, slot, frame->second_field);
  s->field_phase_fail_cnt++;
}

static void test_st20p_rx_frame_thread(St20pContext* s, bool internal_sha) {
  auto handle = static_cast<st20p_rx_handle>(st20p_get_handle(s));
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);
  uint64_t timestamp = 0;
  unsigned char computed_sha[SHA256_DIGEST_LENGTH];

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    struct st_frame* frame = st20p_rx_get_frame(handle);
    if (!frame) {
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait_for(lck, std::chrono::milliseconds(10));
        lck.unlock();
      }
      continue;
    }

    auto* in_use = static_cast<std::atomic<bool>*>(frame->opaque);
    auto release_ext = [&]() {
      if (in_use) {
        EXPECT_TRUE(in_use->exchange(false, std::memory_order_acq_rel));
        in_use = nullptr;
      }
    };

    if (s->user_meta) test_st20p_rx_user_meta(s, frame);

    if (!st_is_frame_complete(frame->status)) {
      s->incomplete_frame_cnt++;
      st20p_rx_put_frame(handle, frame);
      release_ext();
      continue;
    }

    if (frame->data_size != s->frame_size || frame->buffer_size != s->frame_size ||
        frame->width != s->width || frame->height != s->height || frame->fmt != s->fmt ||
        frame->timestamp == timestamp) {
      s->incomplete_frame_cnt++;
    }
    dbg("%s(%d), timestamp %" PRIu64 "\n", __func__, s->idx, frame->timestamp);
    timestamp = frame->timestamp;

    if (s->rx_timing_parser) test_st20p_rx_check_tp(s, frame);
    if (s->field_period_ns) test_st20p_rx_check_field_phase(s, frame);

    if (s->user_timestamp && !s->user_pacing) {
      if (s->pre_timestamp &&
          (static_cast<uint32_t>(frame->timestamp) - s->pre_timestamp) > 4) {
        s->incomplete_frame_cnt++;
        err("%s(%d), frame user timestamp %" PRIu64 " pre_timestamp %u\n", __func__,
            s->idx, frame->timestamp, s->pre_timestamp);
      }
      s->pre_timestamp = static_cast<uint32_t>(frame->timestamp);
    }

    unsigned char* sha = nullptr;
    if (internal_sha) {
      SHA256((unsigned char*)frame->addr[0], s->frame_size, computed_sha);
      sha = computed_sha;
    } else {
      sha = (unsigned char*)frame->addr[0] + frame->data_size - SHA256_DIGEST_LENGTH;
    }

    int i = 0;
    for (; i < TEST_SHA_HIST_NUM; i++) {
      if (!memcmp(sha, s->shas[i], SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= TEST_SHA_HIST_NUM) {
      test_sha_dump("st20p_rx_error_sha", sha);
      s->sha_fail_cnt++;
    }

    /* Return frame to MTL before releasing the dynamic ext slot for reuse. */
    st20p_rx_put_frame(handle, frame);
    release_ext();
    __atomic_fetch_add(&s->fb_rec, 1, __ATOMIC_RELAXED);
    if (!s->start_time) s->start_time = st_test_get_monotonic_time();
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static int test_st20p_rx_query_ext_frame(void* priv, st_ext_frame* ext_frame,
                                         struct st20_rx_frame_meta* /*meta*/) {
  auto* s = static_cast<St20pContext*>(priv);
  const size_t n = s->ext_frames.count();
  if (n == 0) return -EIO;

  const size_t start = static_cast<size_t>(s->ext_idx) % n;
  for (size_t attempt = 0; attempt < n; attempt++) {
    const size_t i = (start + attempt) % n;
    if (s->ext_frames.try_acquire_slot(i)) {
      *ext_frame = s->ext_frames.frames()[i];
      ext_frame->opaque = s->ext_frames.in_use_slot(i);
      s->ext_idx = static_cast<int>((i + 1) % n);
      return 0;
    }
  }

  err("%s(%d), all %zu ext frames in use\n", __func__, s->idx, n);
  return -EIO;
}

static void populate_tx_frames(St20pContext* s, st20p_tx_handle handle,
                               const St20pStreamSpec& spec,
                               const St20pDigestScenario& sc) {
  const uint8_t planes = st_frame_fmt_planes(spec.tx_fmt);
  const size_t frame_size = s->frame_size;

  for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
    uint8_t* fb = sc.tx_ext
                      ? (s->ext_frames.data() + static_cast<size_t>(frame) * frame_size)
                      : static_cast<uint8_t*>(st20p_tx_get_fb_addr(handle, frame));
    ASSERT_TRUE(fb != nullptr);

    if (!sc.line_padding_size) {
      st_test_rand_data(fb, frame_size, frame);
    } else {
      const int data_h = sc.interlace ? spec.height / 2 : spec.height;
      for (uint8_t plane = 0; plane < planes; plane++) {
        const size_t least_line = st_frame_least_linesize(spec.tx_fmt, spec.width, plane);
        uint8_t* start = static_cast<uint8_t*>(s->ext_frames.frames()[frame].addr[plane]);
        for (int line = 0; line < data_h; line++) {
          uint8_t* cur = start + s->ext_frames.frames()[frame].linesize[plane] *
                                     static_cast<size_t>(line);
          st_test_rand_data(cur, least_line, frame);
        }
      }
    }

    if (spec.tx_fmt == ST_FRAME_FMT_YUV422PLANAR10LE) {
      auto* p10 = reinterpret_cast<uint16_t*>(fb);
      for (size_t j = 0; j < frame_size / 2; j++) p10[j] &= 0x3ff;
    } else if (spec.tx_fmt == ST_FRAME_FMT_Y210) {
      auto* y210 = reinterpret_cast<uint16_t*>(fb);
      for (size_t j = 0; j < frame_size / 2; j++) y210[j] &= 0xffc0;
    } else if (spec.tx_fmt == ST_FRAME_FMT_V210) {
      auto* v210 = reinterpret_cast<uint32_t*>(fb);
      for (size_t j = 0; j < frame_size / 4; j++) v210[j] &= 0x3fffffff;
    }

    unsigned char* result = s->shas[frame];
    SHA256(fb, frame_size, result);
    test_sha_dump("st20p_tx", result);
    if (sc.device == ST_PLUGIN_DEVICE_TEST) {
      memcpy(fb + frame_size - SHA256_DIGEST_LENGTH, result, SHA256_DIGEST_LENGTH);
    }
  }
}

static void st20p_rx_digest_test(const St20pDigestScenario& sc) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  if (sc.tx_ext || sc.rx_ext) MTL_TEST_REQUIRE_IOVA_VA(ctx);
  if (sc.pkt_convert) MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_R);

  /* Mutate plugin knobs only after all GTEST_SKIP preconditions pass. */
  mtl_test::ScopedPluginFaults plugin_guard(ctx, sc.fail_interval, sc.timeout_interval,
                                            sc.timeout_ms);

  const size_t sessions = sc.streams.size();
  const uint8_t pt = sc.zero_payload_type ? 0 : ST20P_TEST_PAYLOAD_TYPE;

  /* Declaration order guarantees reverse RAII teardown on any ASSERT_* failure. */
  std::vector<ScopedSt20pContext> tx_ctxs;
  std::vector<ScopedSt20pContext> rx_ctxs;
  std::vector<ScopedSt20pTx> tx_handles(sessions);
  std::vector<ScopedSt20pRx> rx_handles(sessions);
  mtl_test::ScopedMtlStart mtl_run(st);
  std::vector<mtl_test::ScopedWorker> tx_workers;
  std::vector<mtl_test::ScopedWorker> rx_workers;
  std::vector<double> expect_fps_tx(sessions, 0.0);
  std::vector<double> expect_fps_rx(sessions, 0.0);

  tx_ctxs.reserve(sessions);
  rx_ctxs.reserve(sessions);
  tx_workers.reserve(sessions);
  rx_workers.reserve(sessions);

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = sc.streams[i];
    expect_fps_tx[i] = st_frame_rate(spec.fps);
    if (sc.timeout_interval) {
      expect_fps_tx[i] =
          expect_fps_tx[i] * (sc.timeout_interval - 1) / sc.timeout_interval;
    }

    tx_ctxs.push_back(make_st20p_context(ctx, static_cast<int>(i), TEST_SHA_HIST_NUM));
    St20pContext* raw_tx = tx_ctxs.back().get();
    raw_tx->width = spec.width;
    raw_tx->height = spec.height;
    raw_tx->fmt = spec.tx_fmt;
    raw_tx->user_timestamp = sc.user_timestamp;
    raw_tx->user_meta = sc.user_meta;
    raw_tx->block_get = sc.block_get;

    struct st20p_tx_ops ops_tx;
    st20p_tx_ops_init(raw_tx, &ops_tx);
    mtl_test::fill_tx_port(ops_tx.port, ctx,
                           ST20P_TEST_UDP_PORT + static_cast<uint16_t>(i * 2), pt,
                           sc.ssrc);
    ops_tx.width = spec.width;
    ops_tx.height = spec.height;
    ops_tx.fps = spec.fps;
    ops_tx.input_fmt = spec.tx_fmt;
    ops_tx.interlaced = sc.interlace;
    ops_tx.transport_packing = sc.packing;
    ops_tx.transport_pacing = sc.pacing;
    ops_tx.transport_fmt = spec.t_fmt;
    ops_tx.device = sc.device;
    ops_tx.notify_frame_done = test_st20p_tx_frame_done;
    if (sc.block_get) {
      ops_tx.notify_frame_available = nullptr;
      ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;
    }
    if (sc.tx_ext) {
      ops_tx.flags |= ST20P_TX_FLAG_EXT_FRAME | ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE;
    }
    if (sc.user_timestamp) ops_tx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
    if (sc.vsync) ops_tx.flags |= ST20P_TX_FLAG_ENABLE_VSYNC;
    if (sc.rtcp) {
      ops_tx.flags |= ST20P_TX_FLAG_ENABLE_RTCP;
      ops_tx.rtcp.buffer_size = 1024;
    }

    const uint8_t planes = st_frame_fmt_planes(spec.tx_fmt);
    const size_t data_h =
        static_cast<size_t>(sc.interlace ? spec.height / 2 : spec.height);
    raw_tx->frame_size =
        st_frame_size(spec.tx_fmt, spec.width, spec.height, sc.interlace) +
        sc.line_padding_size * data_h * planes;

    tx_handles[i].reset(st20p_tx_create(st, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(tx_handles[i]));
    if (sc.block_get) {
      EXPECT_EQ(st20p_tx_set_block_timeout(tx_handles[i].get(), NS_PER_S), 0);
    }

    int sch = st20p_tx_get_sch_idx(tx_handles[i].get());
    EXPECT_GE(sch, 0);
    EXPECT_GE(mtl_sch_enable_sleep(st, sch, false), 0);

    if (sc.tx_ext) {
      ASSERT_TRUE(raw_tx->ext_frames.init(st, spec.tx_fmt, spec.width, spec.height,
                                          sc.interlace, raw_tx->fb_cnt,
                                          sc.line_padding_size));
      raw_tx->frame_size = raw_tx->ext_frames.frame_size();
      info("%s, session %zu ext_fb %p\n", __func__, i, raw_tx->ext_frames.data());
    }

    populate_tx_frames(raw_tx, tx_handles[i].get(), spec, sc);
    if (::testing::Test::HasFatalFailure()) return;

    st20p_set_handle(raw_tx, tx_handles[i].get());
    st20p_tx_handle raw_tx_h = tx_handles[i].get();
    tx_workers.emplace_back(raw_tx, std::thread(test_st20p_tx_frame_thread, raw_tx),
                            sc.block_get ? std::function<void()>([raw_tx_h]() {
                              st20p_tx_wake_block(raw_tx_h);
                            })
                                         : nullptr);
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& spec = sc.streams[i];
    if (sc.fail_interval) {
      expect_fps_tx[i] = expect_fps_tx[i] * (sc.fail_interval - 1) / sc.fail_interval;
    }
    expect_fps_rx[i] = expect_fps_tx[i];

    rx_ctxs.push_back(make_st20p_context(ctx, static_cast<int>(i), TEST_SHA_HIST_NUM));
    St20pContext* raw_rx = rx_ctxs.back().get();
    raw_rx->width = spec.width;
    raw_rx->height = spec.height;
    raw_rx->fmt = spec.rx_fmt;
    raw_rx->user_timestamp = sc.user_timestamp;
    raw_rx->user_meta = sc.user_meta;
    raw_rx->block_get = sc.block_get;
    raw_rx->rx_timing_parser = sc.rx_timing_parser;
    struct st20p_rx_ops ops_rx;
    st20p_rx_ops_init(raw_rx, &ops_rx);
    raw_rx->frame_size =
        st_frame_size(spec.rx_fmt, spec.width, spec.height, sc.interlace);
    memcpy(raw_rx->shas, tx_ctxs[i]->shas, TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    if (sc.rx_ext) {
      ASSERT_TRUE(raw_rx->ext_frames.init(st, spec.rx_fmt, spec.width, spec.height,
                                          sc.interlace, raw_rx->fb_cnt,
                                          sc.line_padding_size));
      raw_rx->frame_size = raw_rx->ext_frames.frame_size();
      info("%s, session %zu ext_fb %p\n", __func__, i, raw_rx->ext_frames.data());
    }

    mtl_test::fill_rx_port(ops_rx.port, ctx,
                           ST20P_TEST_UDP_PORT + static_cast<uint16_t>(i * 2), pt,
                           sc.ssrc);
    ops_rx.width = spec.width;
    ops_rx.height = spec.height;
    ops_rx.fps = spec.fps;
    ops_rx.output_fmt = spec.rx_fmt;
    ops_rx.transport_fmt = spec.t_fmt;
    ops_rx.interlaced = sc.interlace;
    if (ops_rx.interlaced) {
      /* Must stay a double: rounding 16683333.33ns at 59.94 shifts the slot index. */
      raw_rx->field_period_ns = static_cast<double>(NS_PER_S) / st_frame_rate(spec.fps);
    }
    ops_rx.device = sc.device;
    if (sc.block_get) {
      ops_rx.notify_frame_available = nullptr;
      ops_rx.flags |= ST20P_RX_FLAG_BLOCK_GET;
    }
    if (sc.rx_timing_parser) ops_rx.flags |= ST20P_RX_FLAG_TIMING_PARSER_META;
    if (sc.rx_ext) {
      if (sc.rx_dedicated_ext) {
        ops_rx.ext_frames = raw_rx->ext_frames.frames();
      } else {
        ops_rx.flags |= ST20P_RX_FLAG_EXT_FRAME;
        ops_rx.query_ext_frame = test_st20p_rx_query_ext_frame;
        if (st_frame_fmt_equal_transport(ops_rx.output_fmt, ops_rx.transport_fmt))
          ops_rx.flags |= ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
      }
    }
    if (sc.vsync) ops_rx.flags |= ST20P_RX_FLAG_ENABLE_VSYNC;
    if (sc.pkt_convert) ops_rx.flags |= ST20P_RX_FLAG_PKT_CONVERT;
    if (sc.rx_auto_detect) ops_rx.flags |= ST20P_RX_FLAG_AUTO_DETECT;
    if (sc.rtcp) {
      ops_rx.flags |= ST20P_RX_FLAG_ENABLE_RTCP | ST20P_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 250;
      ops_rx.rtcp.seq_bitmap_size = 64;
      ops_rx.rtcp.seq_skip_window = 0;
      ops_rx.rtcp.burst_loss_max = 1;
      ops_rx.rtcp.sim_loss_rate = 0.1;
    }

    rx_handles[i].reset(st20p_rx_create(st, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(rx_handles[i]));
    if (sc.block_get) {
      EXPECT_EQ(st20p_rx_set_block_timeout(rx_handles[i].get(), NS_PER_S), 0);
    }

    int sch = st20p_rx_get_sch_idx(rx_handles[i].get());
    EXPECT_GE(sch, 0);
    EXPECT_GE(mtl_sch_enable_sleep(st, sch, false), 0);

    st20p_set_handle(raw_rx, rx_handles[i].get());
    st20p_rx_handle raw_rx_h = rx_handles[i].get();
    const bool internal_sha = (sc.device == ST_PLUGIN_DEVICE_TEST_INTERNAL);
    rx_workers.emplace_back(
        raw_rx, std::thread(test_st20p_rx_frame_thread, raw_rx, internal_sha),
        sc.block_get
            ? std::function<void()>([raw_rx_h]() { st20p_rx_wake_block(raw_rx_h); })
            : nullptr);

    struct st_queue_meta meta;
    EXPECT_GE(st20p_rx_get_queue_meta(rx_handles[i].get(), &meta), 0);
  }

  EXPECT_GE(mtl_run.start(), 0);

  std::vector<tests_context*> raw_tx_ctxs(sessions), raw_rx_ctxs(sessions);
  for (size_t i = 0; i < sessions; i++) {
    raw_tx_ctxs[i] = tx_ctxs[i].get();
    raw_rx_ctxs[i] = rx_ctxs[i].get();
  }

  /*
   * Warmup=3 absorbs RL pacing and startup; >=25 frames over >=1000 ms gives
   * <=4.0% edge error at 25 fps and <=1.7% at 59.94 fps (vs +/-10% tolerance)
   * while exercising >=8 fault intervals and hundreds of RTCP NACKs.
   */
  const auto win = mtl_test::measure_pipeline_window(
      raw_tx_ctxs, raw_rx_ctxs, /*warmup_frames=*/3, /*min_delta_frames=*/25,
      /*min_window_ms=*/1000, /*warmup_timeout_ms=*/6000, /*window_timeout_ms=*/6000);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);

  for (size_t i = 0; i < sessions; i++) tx_workers[i].stop();
  for (size_t i = 0; i < sessions; i++) rx_workers[i].stop();
  EXPECT_GE(mtl_run.stop(), 0);

  if (sc.vsync) {
    const bool kernel_lo = mtl_test::is_kernel_loopback(ctx);
    for (size_t i = 0; i < sessions; i++) {
      const double nominal_fps = st_frame_rate(sc.streams[i].fps);
      EXPECT_GT(mtl_test::vsync_count(raw_tx_ctxs[i]), 0);
      EXPECT_NEAR(win.tx_vsync[i], nominal_fps, nominal_fps * 0.1);
      EXPECT_GT(mtl_test::vsync_count(raw_rx_ctxs[i]), 0);
      if (!kernel_lo) {
        EXPECT_NEAR(win.rx_vsync[i], nominal_fps, nominal_fps * 0.1);
      } else {
        info("%s, skip vsync check as it's kernel:lo\n", __func__);
      }
    }
  }

  for (size_t i = 0; i < sessions; i++) {
    tx_handles[i].reset();
    const int sent = mtl_test::tx_frames(raw_tx_ctxs[i]);
    info("%s, session %zu fb_send %d framerate %f:%f\n", __func__, i, sent, win.tx_fps[i],
         expect_fps_tx[i]);
    EXPECT_GT(sent, 0);
    raw_rx_ctxs[i]->fb_send = sent;
  }

  for (size_t i = 0; i < sessions; i++) {
    rx_handles[i].reset();
    const int rec = mtl_test::rx_frames(raw_rx_ctxs[i]);
    info("%s, session %zu fb_rec %d framerate %f:%f\n", __func__, i, rec, win.rx_fps[i],
         expect_fps_rx[i]);
    EXPECT_GT(rec, 0);
    EXPECT_LE(raw_rx_ctxs[i]->incomplete_frame_cnt, 4);
    EXPECT_EQ(raw_rx_ctxs[i]->sha_fail_cnt, 0);
    EXPECT_LE(raw_rx_ctxs[i]->user_meta_fail_cnt, 2);
    EXPECT_EQ(raw_rx_ctxs[i]->tp_fail_cnt, 0);
    EXPECT_EQ(raw_rx_ctxs[i]->field_phase_fail_cnt, 0);
    EXPECT_EQ(raw_rx_ctxs[i]->field_phase_unverified_cnt, 0);
    if (sc.check_fps) {
      const double tol = (sc.fail_interval || sc.timeout_interval) ? 0.5 : 0.1;
      EXPECT_NEAR(win.rx_fps[i], expect_fps_rx[i], expect_fps_rx[i] * tol);
    }
    if (sc.rtcp) {
      info("%s, session %zu rx/tx fb ratio %f\n", __func__, i,
           static_cast<double>(rec) / raw_rx_ctxs[i]->fb_send);
    }
  }
}

TEST(St20p, digest_1080p_s1) {
  St20pDigestScenario sc;
  sc.streams = {make_stream(ST_FPS_P25, 1280, 720, ST_FRAME_FMT_YUV422PLANAR10LE)};
  sc.check_fps = false;
  sc.rx_timing_parser = true;
  sc.rx_auto_detect = true;
  sc.zero_payload_type = true;
  st20p_rx_digest_test(sc);
}

TEST(St20p, digest_1080i_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10),
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.interlace = true;
  sc.check_fps = false;
  sc.ssrc = 54321;
  sc.block_get = true;
  /* runs the library's own field-parity verdict against a real interlaced stream */
  sc.rx_timing_parser = true;
  st20p_rx_digest_test(sc);
}

TEST(St20p, digest_1080p_fail_interval) {
  St20pDigestScenario sc;
  sc.streams = {make_stream(ST_FPS_P25, 1280, 720, ST_FRAME_FMT_YUV422PLANAR10LE)};
  sc.fail_interval = 3;
  st20p_rx_digest_test(sc);
}

TEST(St20p, digest_1080p_timeout_interval) {
  St20pDigestScenario sc;
  sc.streams = {make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE)};
  sc.timeout_interval = 3;
  sc.timeout_ms = 20;
  st20p_rx_digest_test(sc);
}

TEST(St20p, digest_1080p_internal_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_Y210),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.check_fps = false;
  st20p_rx_digest_test(sc);
}

TEST(St20p, digest_1080p_no_convert_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                  ST20_FMT_YUV_422_10BIT),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  /* Dual 1080p59.94 (~6.56 Gbps incl. RGB8) + SHA256 exceeds single-lcore TSC pacing. */
  if (st_test_ctx()->para.pacing == ST21_TX_PACING_WAY_TSC) sc.check_fps = false;
  st20p_rx_digest_test(sc);
}

TEST(St20p, digest_1080p_packet_convert_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_Y210),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.check_fps = false;
  sc.pkt_convert = true;
  st20p_rx_digest_test(sc);
}

TEST(St20p, rx_ext_digest_1080p_no_convert_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.rx_ext = true;
  sc.block_get = true;
  sc.rx_auto_detect = true;
  st20p_rx_digest_test(sc);
}

/* Packet convert writes each packet into the dst frame as it arrives, but a
 * dynamic ext frame can only be lent once its frame is known to be delivered. */
TEST(St20p, rx_create_expect_fail_packet_convert_ext_frame) {
  auto ctx = st_test_ctx();
  auto test_ctx = make_st20p_context(ctx, 0, 2);

  struct st20p_rx_ops ops;
  st20p_rx_ops_init(test_ctx.get(), &ops);
  ops.flags |= ST20P_RX_FLAG_PKT_CONVERT | ST20P_RX_FLAG_EXT_FRAME;
  ops.query_ext_frame = test_st20p_rx_query_ext_frame;

  ScopedSt20pRx handle(st20p_rx_create(ctx->handle, &ops));
  EXPECT_FALSE(static_cast<bool>(handle));
}

TEST(St20p, ext_digest_1080p_no_convert_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.tx_ext = true;
  sc.rx_ext = true;
  sc.check_fps = false;
  st20p_rx_digest_test(sc);
}

TEST(St20p, ext_digest_1080p_convert_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_V210),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.tx_ext = true;
  sc.rx_ext = true;
  sc.check_fps = false;
  sc.user_timestamp = true;
  sc.block_get = false;
  st20p_rx_digest_test(sc);
}

TEST(St20p, ext_digest_1080p_convert_with_padding_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_Y210),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.tx_ext = true;
  sc.rx_ext = true;
  sc.check_fps = false;
  sc.line_padding_size = 1024;
  st20p_rx_digest_test(sc);
}

TEST(St20p, ext_digest_1080p_packet_convert_with_padding_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_Y210),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.tx_ext = true;
  sc.rx_ext = true;
  sc.rx_dedicated_ext = true;
  sc.check_fps = false;
  sc.line_padding_size = 1024;
  sc.pkt_convert = true;
  st20p_rx_digest_test(sc);
}

TEST(St20p, digest_user_meta_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10),
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.user_meta = true;
  sc.check_fps = false;
  sc.packing = ST20_PACKING_GPM;
  sc.block_get = true;
  st20p_rx_digest_test(sc);
}

TEST(St20p, digest_rtcp_s1) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10)};
  sc.rtcp = true;
  sc.check_fps = false;
  sc.packing = ST20_PACKING_GPM_SL;
  st20p_rx_digest_test(sc);
}

TEST(St20p, transport_yuv422p10le) {
  St20pDigestScenario sc;
  sc.streams = {make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE,
                            ST20_FMT_YUV_422_PLANAR10LE)};
  sc.packing = ST20_PACKING_BPM;
  st20p_rx_digest_test(sc);
}

/*
 * Regression test for ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE's two-phase
 * release contract: a slot held past notify_frame_done must stay IN_USER
 * (unreclaimable by st20p_tx_get_frame) until the app explicitly calls
 * st20p_tx_notify_ext_frame_free() on it.
 */
TEST(St20p, tx_ext_frame_manual_release_two_phase) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  MTL_TEST_REQUIRE_IOVA_VA(ctx);

  const int fb_cnt = 3;
  const int width = 1920, height = 1080;
  /* Single-plane format equal to transport format so derive=true and frames
   * complete via tx_st20p_frame_done, parking IN_USER until manual release. */
  const enum st_frame_fmt fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;

  tx_ext_release_ctx tctx;
  mtl_test::ScopedExtFrames ext_bufs;
  ASSERT_TRUE(ext_bufs.init(st, fmt, width, height, false, fb_cnt));

  auto dummy_ctx = make_st20p_context(ctx, 0, fb_cnt);
  struct st20p_tx_ops ops_tx;
  st20p_tx_ops_init(dummy_ctx.get(), &ops_tx);
  ops_tx.name = "st20p_tx_ext_release_test";
  ops_tx.priv = &tctx;
  ops_tx.input_fmt = fmt;
  ops_tx.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  ops_tx.notify_frame_available = nullptr;
  ops_tx.notify_event = nullptr;
  ops_tx.flags = ST20P_TX_FLAG_EXT_FRAME | ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE |
                 ST20P_TX_FLAG_BLOCK_GET;
  ops_tx.notify_frame_done = test_tx_ext_release_frame_done;

  ScopedSt20pTx tx_handle(st20p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  tctx.handle.store(tx_handle.get(), std::memory_order_release);
  EXPECT_EQ(st20p_tx_set_block_timeout(tx_handle.get(), NS_PER_S / 5), 0); /* 200ms */

  mtl_test::ScopedMtlStart mtl_run(st);
  EXPECT_GE(mtl_run.start(), 0);

  /* Prime all fb_cnt slots so each has been claimed exactly once. */
  std::set<struct st_frame*> primed_frames;
  int ext_idx = 0;
  for (int i = 0; i < fb_cnt; i++) {
    struct st_frame* frame = st20p_tx_get_frame(tx_handle.get());
    ASSERT_TRUE(frame != nullptr);
    primed_frames.insert(frame);
    EXPECT_GE(st20p_tx_put_ext_frame(tx_handle.get(), frame,
                                     &ext_bufs.frames()[ext_idx++ % fb_cnt]),
              0);
  }
  ASSERT_EQ(static_cast<int>(primed_frames.size()), fb_cnt);

  /* Steady state: one primed slot gets held forever by notify_frame_done,
   * so get_frame must never reclaim more than fb_cnt - 1 distinct slots. */
  std::set<struct st_frame*> steady_frames;
  const int steady_cycles = 30;
  for (int i = 0; i < steady_cycles; i++) {
    struct st_frame* frame = st20p_tx_get_frame(tx_handle.get());
    if (!frame) continue; /* timed out waiting on the held slot, expected */
    steady_frames.insert(frame);
    EXPECT_GE(st20p_tx_put_ext_frame(tx_handle.get(), frame,
                                     &ext_bufs.frames()[ext_idx++ % fb_cnt]),
              0);
  }

  ASSERT_TRUE(tctx.held_frame.load() != nullptr);
  EXPECT_EQ(static_cast<int>(steady_frames.size()), fb_cnt - 1);
  EXPECT_EQ(steady_frames.count(tctx.held_frame.load()), 0u);

  /* Explicit release: only now can the held slot be reclaimed. */
  EXPECT_GE(st20p_tx_notify_ext_frame_free(tx_handle.get(), tctx.held_frame.load()), 0);

  bool held_reclaimed = false;
  for (int i = 0; i < steady_cycles && !held_reclaimed; i++) {
    struct st_frame* frame = st20p_tx_get_frame(tx_handle.get());
    if (!frame) continue;
    if (frame == tctx.held_frame.load()) held_reclaimed = true;
    EXPECT_GE(st20p_tx_put_ext_frame(tx_handle.get(), frame,
                                     &ext_bufs.frames()[ext_idx++ % fb_cnt]),
              0);
  }
  EXPECT_TRUE(held_reclaimed);
  EXPECT_GE(mtl_run.stop(), 0);
}

namespace {

struct St20pLoopbackOpts {
  uint32_t rx_extra_flags = 0;
  int width = 1280;
  int height = 720;
  enum st_fps fps = ST_FPS_P59_94;
  bool redundant = false;
  bool start_tx_worker = true;
  bool start_rx_worker = true;
  bool start_mtl = true;
};

/* Lightweight RAII loopback pair for ST20p control/stats/pcap/abort API tests. */
struct St20pLoopbackPair {
  ScopedSt20pContext tx_ctx;
  ScopedSt20pContext rx_ctx;
  ScopedSt20pTx tx;
  ScopedSt20pRx rx;
  mtl_test::ScopedMtlStart mtl_run;
  mtl_test::ScopedWorker tx_worker;
  mtl_test::ScopedWorker rx_worker;

  explicit St20pLoopbackPair(mtl_handle st) : mtl_run(st) {
  }

  void init(struct st_tests_context* ctx, uint16_t udp_port,
            const St20pLoopbackOpts& opts = {}) {
    tx_ctx = make_st20p_context(ctx, 0, 3);
    rx_ctx = make_st20p_context(ctx, 0, 3);

    struct st20p_tx_ops ops_tx;
    st20p_tx_ops_init(tx_ctx.get(), &ops_tx);
    ops_tx.name = "st20p_api_tx";
    mtl_test::fill_tx_port(ops_tx.port, ctx, udp_port, ST20P_TEST_PAYLOAD_TYPE, 0,
                           opts.redundant);
    ops_tx.width = opts.width;
    ops_tx.height = opts.height;
    ops_tx.fps = opts.fps;
    ops_tx.input_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
    ops_tx.transport_fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
    ops_tx.notify_frame_available = nullptr;
    ops_tx.notify_event = nullptr;
    ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;

    tx.reset(st20p_tx_create(ctx->handle, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(tx));
    EXPECT_EQ(st20p_tx_set_block_timeout(tx.get(), NS_PER_S), 0);
    st20p_set_handle(tx_ctx.get(), tx.get());

    struct st20p_rx_ops ops_rx;
    st20p_rx_ops_init(rx_ctx.get(), &ops_rx);
    ops_rx.name = "st20p_api_rx";
    mtl_test::fill_rx_port(ops_rx.port, ctx, udp_port, ST20P_TEST_PAYLOAD_TYPE, 0,
                           opts.redundant);
    ops_rx.width = opts.width;
    ops_rx.height = opts.height;
    ops_rx.fps = opts.fps;
    ops_rx.output_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
    ops_rx.transport_fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
    ops_rx.notify_frame_available = nullptr;
    ops_rx.notify_event = nullptr;
    ops_rx.flags |= ST20P_RX_FLAG_BLOCK_GET | opts.rx_extra_flags;

    rx.reset(st20p_rx_create(ctx->handle, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(rx));
    EXPECT_EQ(st20p_rx_set_block_timeout(rx.get(), NS_PER_S), 0);
    st20p_set_handle(rx_ctx.get(), rx.get());

    if (opts.start_tx_worker) start_tx();
    if (opts.start_rx_worker) start_rx();
    if (opts.start_mtl) {
      EXPECT_GE(mtl_run.start(), 0);
    }
  }

  void start_tx() {
    st20p_tx_handle tx_h = tx.get();
    tx_worker = mtl_test::ScopedWorker(
        tx_ctx.get(),
        std::thread(
            [](tests_context* s) {
              auto handle = static_cast<st20p_tx_handle>(st20p_get_handle(s));
              while (!s->stop) {
                auto* frame = st20p_tx_get_frame(handle);
                if (!frame) continue;
                st20p_tx_put_frame(handle, frame);
                __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
              }
            },
            tx_ctx.get()),
        [tx_h]() { st20p_tx_wake_block(tx_h); });
  }

  void start_rx() {
    st20p_rx_handle rx_h = rx.get();
    rx_worker = mtl_test::ScopedWorker(
        rx_ctx.get(),
        std::thread(
            [](tests_context* s) {
              auto handle = static_cast<st20p_rx_handle>(st20p_get_handle(s));
              while (!s->stop) {
                auto* frame = st20p_rx_get_frame(handle);
                if (!frame) continue;
                const bool complete = st_is_frame_complete(frame->status);
                st20p_rx_put_frame(handle, frame);
                if (complete) __atomic_fetch_add(&s->fb_rec, 1, __ATOMIC_RELAXED);
              }
            },
            rx_ctx.get()),
        [rx_h]() { st20p_rx_wake_block(rx_h); });
  }

  void stop_all() {
    tx_worker.stop();
    rx_worker.stop();
    EXPECT_GE(mtl_run.stop(), 0);
  }
};

}  // namespace

TEST(St20p, put_frame_abort) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  St20pLoopbackPair pair(ctx->handle);
  St20pLoopbackOpts opts;
  opts.start_tx_worker = false;
  opts.start_rx_worker = false;
  opts.start_mtl = false;
  pair.init(ctx, ST20P_TEST_UDP_PORT, opts);
  if (::testing::Test::HasFatalFailure()) return;

  /* Verify TX abort returns the frame without transmitting. */
  struct st_frame* tx_frame = st20p_tx_get_frame(pair.tx.get());
  ASSERT_NE(tx_frame, nullptr);
  EXPECT_GE(st20p_tx_put_frame_abort(pair.tx.get(), tx_frame), 0);

  /* Start TX and verify RX abort releases an incoming frame cleanly. */
  pair.start_tx();
  EXPECT_GE(pair.mtl_run.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::tx_frames(pair.tx_ctx.get()) >= 10; }, 5000));

  struct st_frame* rx_frame = nullptr;
  mtl_test::wait_until(
      [&]() {
        rx_frame = st20p_rx_get_frame(pair.rx.get());
        return rx_frame != nullptr;
      },
      5000);
  ASSERT_NE(rx_frame, nullptr);
  EXPECT_GE(st20p_rx_put_frame_abort(pair.rx.get(), rx_frame), 0);

  pair.stop_all();
}

/*
 * Verify redundant TX and RX port stats for st20p video sessions.
 * Requires 4 ports: TX on ports 0+1, RX on ports 2+3.
 */
TEST(St20p, redundant_stats) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 4);

  St20pLoopbackPair pair(ctx->handle);
  St20pLoopbackOpts opts;
  opts.redundant = true;
  opts.width = 1920;
  opts.height = 1080;
  pair.init(ctx, ST20P_TEST_UDP_PORT + 100, opts);
  if (::testing::Test::HasFatalFailure()) return;

  /* Wait for both redundant RX legs to report traffic, then accumulate >= 50 frames
   * (~0.83 s at 59.94 fps) so startup skew cannot distort the 80% byte-balance check. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        struct st20_rx_user_stats probe;
        if (st20p_rx_get_session_stats(pair.rx.get(), &probe) < 0) return false;
        return probe.common.port[MTL_SESSION_PORT_P].bytes > 0 &&
               probe.common.port[MTL_SESSION_PORT_R].bytes > 0;
      },
      6000));
  const int base_rx = mtl_test::rx_frames(pair.rx_ctx.get());
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(pair.rx_ctx.get()) >= base_rx + 50; }, 6000));

  pair.stop_all();

  struct st20_tx_user_stats tx_stats;
  struct st20_rx_user_stats rx_stats;
  EXPECT_GE(st20p_tx_get_session_stats(pair.tx.get(), &tx_stats), 0);
  EXPECT_GE(st20p_rx_get_session_stats(pair.rx.get(), &rx_stats), 0);
  mtl_test::expect_redundant_port_stats(tx_stats, rx_stats);

  EXPECT_GT(mtl_test::rx_frames(pair.rx_ctx.get()), 0) << "Must have received frames";
  info("%s, TX sent %d, RX received %d frames\n", __func__,
       mtl_test::tx_frames(pair.tx_ctx.get()), mtl_test::rx_frames(pair.rx_ctx.get()));
}

/*
 * Concurrency reproducer: get/reset_session_stats racing with _free().
 * Handle refcounting ensures readers either complete safely or receive -EIO.
 */
static void st20p_rx_stats_concurrent_free_test(bool reset, int reader_threads = 8,
                                                int iterations = 100) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  auto test_ctx = mtl_test::make_context(ctx, 0, 2);

  struct st20p_rx_ops ops;
  st20p_rx_ops_init(test_ctx.get(), &ops);

  std::atomic<st20p_rx_handle> live{nullptr};
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> calls{0};

  auto reader = [&]() {
    struct st20_rx_user_stats stats;
    while (!stop.load(std::memory_order_acquire)) {
      auto h = live.load(std::memory_order_acquire);
      if (!h) {
        std::this_thread::yield();
        continue;
      }
      for (int i = 0; i < 16; i++) {
        int ret = reset ? st20p_rx_reset_session_stats(h)
                        : st20p_rx_get_session_stats(h, &stats);
        calls.fetch_add(1, std::memory_order_relaxed);
        if (ret < 0) {
          live.compare_exchange_strong(h, nullptr);
          break;
        }
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(reader_threads));
  struct ReaderJoinGuard {
    std::atomic<bool>& stop;
    std::vector<std::thread>& threads;
    ~ReaderJoinGuard() {
      stop.store(true, std::memory_order_release);
      for (auto& t : threads) {
        if (t.joinable()) t.join();
      }
    }
  } join_guard{stop, threads};

  for (int i = 0; i < reader_threads; i++) threads.emplace_back(reader);

  for (int it = 0; it < iterations; it++) {
    auto handle = st20p_rx_create(m_handle, &ops);
    ASSERT_TRUE(handle != nullptr) << "iter " << it;
    live.store(handle, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    live.store(nullptr, std::memory_order_release);
    EXPECT_GE(st20p_rx_free(handle), 0) << "iter " << it;
  }

  stop.store(true, std::memory_order_release);
  for (auto& t : threads) {
    if (t.joinable()) t.join();
  }

  info("%s, total %s calls %" PRIu64 " over %d iters / %d threads\n", __func__,
       reset ? "reset_stats" : "get_stats", calls.load(), iterations, reader_threads);
}

TEST(St20p, rx_get_stats_concurrent_free_no_uaf) {
  st20p_rx_stats_concurrent_free_test(false);
}

TEST(St20p, rx_reset_stats_concurrent_free_no_uaf) {
  st20p_rx_stats_concurrent_free_test(true);
}

static void st20p_tx_epoch_drop_test(bool user_pacing) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  auto test_ctx = mtl_test::make_context(ctx, 0, 3);

  struct st20p_tx_ops ops_tx;
  st20p_tx_ops_init(test_ctx.get(), &ops_tx);
  ops_tx.name = "st20p_epoch_drop_test";
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = EPOCH_DROP_TEST_UDP_PORT_ST20P;
  ops_tx.notify_frame_available = nullptr;
  ops_tx.notify_event = nullptr;
  ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;
  if (user_pacing) {
    ops_tx.flags |= ST20P_TX_FLAG_USER_PACING | ST20P_TX_FLAG_USER_TIMESTAMP;
  }

  /* Create session after mtl_start while preserving worker -> mtl_stop -> free RAII. */
  ScopedSt20pTx tx_handle;
  mtl_test::ScopedMtlStart mtl_run(st);
  EXPECT_GE(mtl_run.start(), 0);

  tx_handle.reset(st20p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st20p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);

  st20p_set_handle(test_ctx.get(), tx_handle.get());
  mtl_test::ScopedWorker tx_worker(
      test_ctx.get(),
      std::thread(
          [user_pacing](tests_context* s) {
            auto handle = static_cast<st20p_tx_handle>(st20p_get_handle(s));
            auto m_handle = s->ctx->handle;
            while (!s->stop) {
              auto frame = st20p_tx_get_frame(handle);
              if (!frame) continue;
              if (user_pacing) {
                frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
                frame->timestamp =
                    mtl_ptp_read_time(m_handle) + 20 * 1000 * 1000; /* +20ms */
              }
              st20p_tx_put_frame(handle, frame);
              __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
            }
          },
          test_ctx.get()),
      [&]() { st20p_tx_wake_block(tx_handle.get()); });

  /* 40 paced frames (~0.67 s at 59.94 fps) exercises >13 wraparounds of the 3-slot ring.
   */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::tx_frames(test_ctx.get()) >= 40; }, 6000));

  tx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);

  struct st20_tx_user_stats stats;
  EXPECT_GE(st20p_tx_get_session_stats(tx_handle.get(), &stats), 0);
  EXPECT_GT(mtl_test::tx_frames(test_ctx.get()), 0);
  info("%s, user_pacing %d, fb_send %d, epoch_drop %" PRIu64 "\n", __func__, user_pacing,
       mtl_test::tx_frames(test_ctx.get()), stats.common.stat_epoch_drop);
  EXPECT_EQ(stats.common.stat_epoch_drop, static_cast<uint64_t>(0));
}

TEST(St20p, tx_no_epoch_drop) {
  st20p_tx_epoch_drop_test(false);
}

TEST(St20p, tx_user_pacing_no_epoch_drop) {
  st20p_tx_epoch_drop_test(true);
}

TEST(St20p, query_params) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  const uint16_t fb_cnt = 3;

  auto tx_ctx = mtl_test::make_context(ctx, 0, fb_cnt);
  auto rx_ctx = mtl_test::make_context(ctx, 1, fb_cnt);

  struct st20p_tx_ops ops_tx;
  st20p_tx_ops_init(tx_ctx.get(), &ops_tx);
  ScopedSt20pTx tx_handle(st20p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));

  const size_t expected_tx_size =
      st_frame_size(ops_tx.input_fmt, ops_tx.width, ops_tx.height, ops_tx.interlaced);
  EXPECT_EQ(st20p_tx_frame_size(tx_handle.get()), expected_tx_size);
  EXPECT_GE(st20p_tx_get_sch_idx(tx_handle.get()), 0);
  for (uint16_t idx = 0; idx < fb_cnt; idx++) {
    EXPECT_NE(st20p_tx_get_fb_addr(tx_handle.get(), idx), nullptr);
  }
  EXPECT_EQ(st20p_tx_get_fb_addr(tx_handle.get(), fb_cnt), nullptr);

  double tr_offset_ns = 0.0;
  double trs_ns = 0.0;
  uint32_t vrx_pkts = 0;
  EXPECT_EQ(
      st20p_tx_get_pacing_params(tx_handle.get(), &tr_offset_ns, &trs_ns, &vrx_pkts), 0);
  EXPECT_GT(tr_offset_ns, 0.0);
  EXPECT_GT(trs_ns, 0.0);
  EXPECT_GT(vrx_pkts, 0u);
  EXPECT_EQ(st20p_tx_get_pacing_params(tx_handle.get(), &tr_offset_ns, nullptr, nullptr),
            0);
  EXPECT_LT(st20p_tx_get_pacing_params(nullptr, &tr_offset_ns, &trs_ns, &vrx_pkts), 0);

  struct st20p_rx_ops ops_rx;
  st20p_rx_ops_init(rx_ctx.get(), &ops_rx);
  snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ScopedSt20pRx rx_handle(st20p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));

  const size_t expected_rx_size =
      st_frame_size(ops_rx.output_fmt, ops_rx.width, ops_rx.height, ops_rx.interlaced);
  EXPECT_EQ(st20p_rx_frame_size(rx_handle.get()), expected_rx_size);
  EXPECT_GE(st20p_rx_get_sch_idx(rx_handle.get()), 0);
  for (uint16_t idx = 0; idx < fb_cnt; idx++) {
    EXPECT_NE(st20p_rx_get_fb_addr(rx_handle.get(), idx), nullptr);
  }
  EXPECT_EQ(st20p_rx_get_fb_addr(rx_handle.get(), fb_cnt), nullptr);
  EXPECT_EQ(st20p_rx_get_fb_addr(rx_handle.get(), static_cast<uint16_t>(fb_cnt * 2)),
            nullptr);

  struct st20_rx_tp_pass pass = {};
  EXPECT_LT(st20p_rx_timing_parser_critical(rx_handle.get(), &pass), 0);

  ScopedStFrame hp_frame(
      st_frame_create(st, ST_FRAME_FMT_YUV422PLANAR10LE, 640, 360, false));
  ASSERT_TRUE(static_cast<bool>(hp_frame));
  EXPECT_EQ(st_frame_sanity_check(hp_frame.get()), 0);
  EXPECT_NE(st_frame_addr(hp_frame.get(), 0), nullptr);
  EXPECT_NE(st_frame_addr(hp_frame.get(), 1), nullptr);
  EXPECT_NE(st_frame_addr(hp_frame.get(), 2), nullptr);
  EXPECT_EQ(st_frame_iova(hp_frame.get(), 0),
            mtl_hp_virt2iova(st, st_frame_addr(hp_frame.get(), 0)));
  EXPECT_EQ(hp_frame.get()->buffer_size,
            st_frame_size(ST_FRAME_FMT_YUV422PLANAR10LE, 640, 360, false));
  EXPECT_EQ(hp_frame.get()->data_size, hp_frame.get()->buffer_size);
  hp_frame.reset();
}

TEST(St20p, session_stats_reset) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  St20pLoopbackPair pair(ctx->handle);
  pair.init(ctx, ST20P_TEST_UDP_PORT + 110);
  if (::testing::Test::HasFatalFailure()) return;

  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(pair.rx_ctx.get()) >= 5; }, 6000));
  pair.stop_all();

  struct st20_tx_user_stats tx_stats = {};
  struct st20_rx_user_stats rx_stats = {};
  ASSERT_GE(st20p_tx_get_session_stats(pair.tx.get(), &tx_stats), 0);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GE(st20p_tx_reset_session_stats(pair.tx.get()), 0);
  ASSERT_GE(st20p_tx_get_session_stats(pair.tx.get(), &tx_stats), 0);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);

  /* Free TX before resetting RX stats so no packets arrive under auto-start mode. */
  pair.tx.reset();

  ASSERT_GE(st20p_rx_get_session_stats(pair.rx.get(), &rx_stats), 0);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_GE(st20p_rx_reset_session_stats(pair.rx.get()), 0);
  ASSERT_GE(st20p_rx_get_session_stats(pair.rx.get(), &rx_stats), 0);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
}

TEST(St20p, update_source_and_destination) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const uint16_t udp_port_1 = ST20P_TEST_UDP_PORT + 120;
  const uint16_t udp_port_2 = ST20P_TEST_UDP_PORT + 122;
  const bool use_mcast_phase2 = ctx->mcast_only || !mtl_test::is_kernel_loopback(ctx);

  St20pLoopbackPair pair(ctx->handle);
  pair.init(ctx, udp_port_1);
  if (::testing::Test::HasFatalFailure()) return;

  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(pair.rx_ctx.get()) >= 5; }, 6000));

  auto switch_and_wait = [&](uint16_t udp_port, bool use_mcast) {
    struct st_tx_dest_info dst = {};
    struct st_rx_source_info src = {};
    dst.udp_port[MTL_SESSION_PORT_P] = udp_port;
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    const uint8_t* tx_ip =
        use_mcast ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_R];
    const uint8_t* rx_ip =
        use_mcast ? ctx->mcast_ip_addr[MTL_PORT_P] : ctx->para.sip_addr[MTL_PORT_P];
    memcpy(dst.dip_addr[MTL_SESSION_PORT_P], tx_ip, MTL_IP_ADDR_LEN);
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], rx_ip, MTL_IP_ADDR_LEN);
    EXPECT_GE(st20p_tx_update_destination(pair.tx.get(), &dst), 0);
    EXPECT_GE(st20p_rx_update_source(pair.rx.get(), &src), 0);

    const int base_rx = mtl_test::rx_frames(pair.rx_ctx.get());
    EXPECT_TRUE(mtl_test::wait_until(
        [&]() { return mtl_test::rx_frames(pair.rx_ctx.get()) >= base_rx + 5; }, 6000));
  };

  switch_and_wait(udp_port_2, use_mcast_phase2);
  switch_and_wait(udp_port_1, ctx->mcast_only);
  pair.stop_all();
}

TEST(St20p, rx_timing_parser_critical) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  St20pLoopbackPair pair(ctx->handle);
  St20pLoopbackOpts opts;
  opts.rx_extra_flags = ST20P_RX_FLAG_TIMING_PARSER_META;
  pair.init(ctx, ST20P_TEST_UDP_PORT + 130, opts);
  if (::testing::Test::HasFatalFailure()) return;

  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(pair.rx_ctx.get()) >= 5; }, 6000));

  struct st20_rx_tp_pass pass = {};
  EXPECT_EQ(st20p_rx_timing_parser_critical(pair.rx.get(), &pass), 0);
  EXPECT_GT(pass.cinst_max_narrow, 0);
  EXPECT_GE(pass.cinst_max_wide, pass.cinst_max_narrow);
  EXPECT_GT(pass.vrx_max_narrow, 0);
  EXPECT_GE(pass.vrx_max_wide, pass.vrx_max_narrow);
  EXPECT_GT(pass.tr_offset, 0);
  EXPECT_GT(pass.latency_max, 0);
  EXPECT_GE(pass.rtp_ts_delta_max, pass.rtp_ts_delta_min);

  pair.stop_all();
}

TEST(St20p, rx_pcapng_dump) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  if (!mtl_pmd_is_dpdk_based(ctx->handle, MTL_PORT_R)) {
    GTEST_SKIP() << "MTL_PORT_R is not a DPDK-based PMD";
  }

  St20pLoopbackPair pair(ctx->handle);
  pair.init(ctx, ST20P_TEST_UDP_PORT + 140);
  if (::testing::Test::HasFatalFailure()) return;

  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(pair.rx_ctx.get()) >= 5; }, 6000));

  const uint32_t max_dump_packets = 100;
  struct st_pcap_dump_meta meta = {};
  int ret = st20p_rx_pcapng_dump(pair.rx.get(), max_dump_packets, true, &meta);

  struct UnlinkGuard {
    const char* path = nullptr;
    ~UnlinkGuard() {
      if (path && path[0] != '\0') unlink(path);
    }
  } unlink_guard{meta.file_name[MTL_SESSION_PORT_P]};

  EXPECT_GE(ret, 0);
  EXPECT_EQ(meta.dumped_packets[MTL_SESSION_PORT_P], max_dump_packets);
  if (ret >= 0) {
    struct stat st_buf = {};
    EXPECT_EQ(stat(meta.file_name[MTL_SESSION_PORT_P], &st_buf), 0);
    EXPECT_GT(st_buf.st_size, 0);
  }

  pair.stop_all();
}
