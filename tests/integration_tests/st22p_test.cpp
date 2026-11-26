/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "harness/mock_plugin.hpp"
#include "log.h"
#include "test_harness.hpp"

#define ST22P_TEST_PAYLOAD_TYPE (114)
#define ST22P_TEST_UDP_PORT (16000)

namespace {

using ScopedSt22pTx = mtl_test::ScopedHandle<st22p_tx_handle, st22p_tx_free>;
using ScopedSt22pRx = mtl_test::ScopedHandle<st22p_rx_handle, st22p_rx_free>;
using ScopedSt22EncoderDev =
    mtl_test::ScopedHandle<st22_encoder_dev_handle, st22_encoder_unregister>;
using ScopedSt22DecoderDev =
    mtl_test::ScopedHandle<st22_decoder_dev_handle, st22_decoder_unregister>;

struct UnlinkGuard {
  const char* path = nullptr;
  ~UnlinkGuard() {
    if (path && path[0] != '\0') unlink(path);
  }
};

/* Extended test context owning atomic, dynamically sized external frame slots. */
struct St22pContext : public tests_context {
  mtl_test::ScopedExtFrames ext_frames;
};

struct St22pContextDeleter {
  void operator()(St22pContext* c) const {
    if (!c) return;
    tests_context_unit(c);
    delete c;
  }
};

using ScopedSt22pContext = std::unique_ptr<St22pContext, St22pContextDeleter>;

ScopedSt22pContext make_st22p_context(struct st_tests_context* global_ctx, int idx = 0,
                                      uint16_t fb_cnt = 3, bool check_sha = false) {
  ScopedSt22pContext c(new St22pContext());
  c->ctx = global_ctx;
  c->idx = idx;
  c->fb_cnt = fb_cnt;
  c->fb_idx = 0;
  c->check_sha = check_sha;
  return c;
}

uint64_t next_prng_u64() {
  thread_local uint64_t state = 0x9e3779b97f4a7c15ULL;
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * 0x2545f4914f6cdd1dULL;
}

inline int atomic_inc(int* p) {
  return __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
}

inline void set_handle(tests_context* c, void* h) {
  __atomic_store_n(&c->handle, h, __ATOMIC_RELEASE);
}

inline void* get_handle(const tests_context* c) {
  return __atomic_load_n(&c->handle, __ATOMIC_ACQUIRE);
}

/*
 * Steady-state measurement window for ST 2110-22 pipeline (50-59.94 fps):
 *   - warmup_frames = 3 absorbs pipeline bring-up and RL pacing training.
 *   - min_window_ms = 1000, min_delta_frames = 25 collects 50-60 frames and
 *     vsync ticks at 50-59.94 fps (<= 2.0% edge quantization error, inside
 *     +/-10% FPS and vsync tolerances) and exercises >= 18 fault intervals.
 */
constexpr int kSt22pRateWarmupFrames = 3;
constexpr int kSt22pRateMinDeltaFrames = 25;
constexpr int kSt22pRateMinWindowMs = 1000;

/* Member order guarantees RAII teardown: handle free -> DMA unmap -> context free. */
struct St22pTxSession {
  ScopedSt22pContext ctx;
  ScopedSt22pTx handle;
};

struct St22pRxSession {
  ScopedSt22pContext ctx;
  ScopedSt22pRx handle;
};

template <typename SessionVec>
std::vector<tests_context*> collect_ctxs(const SessionVec& sessions) {
  std::vector<tests_context*> ptrs;
  ptrs.reserve(sessions.size());
  for (const auto& s : sessions) ptrs.push_back(s.ctx.get());
  return ptrs;
}

struct St22pStreamSpec {
  enum st_fps fps = ST_FPS_P59_94;
  int width = 1920;
  int height = 1080;
  enum st_frame_fmt fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  enum st22_codec codec = ST22_CODEC_JPEGXS;
  int compress_ratio = 10;
};

struct St22pDigestScenario {
  std::vector<St22pStreamSpec> streams;
  int fail_interval = 0;
  int timeout_interval = 0;
  int timeout_ms = 0;
  int rand_ratio = 0;
  bool check_fps = true;
  bool user_timestamp = false;
  bool vsync = true;
  bool rtcp = false;
  bool tx_ext = false;
  bool rx_ext = false;
  bool interlace = false;
  uint32_t ssrc = 0;
  bool block_get = false;
  bool codec_block_get = false;
  bool derive = false;
};

struct St22EncoderTraits {
  using SessionHandle = st22p_encode_session;
  using PrivHandle = st22_encode_priv;
  using CreateReq = struct st22_encoder_create_req;
  using FrameMeta = struct st22_encode_frame_meta;

  static bool use_block_get(struct st_tests_context* ctx) {
    return ctx->encoder_use_block_get;
  }

  static void on_create_req(struct st_tests_context* ctx, CreateReq* req,
                            int& sleep_time_us) {
    req->max_codestream_size = req->codestream_size;
    if (ctx->encoder_use_block_get) req->resp_flag |= ST22_ENCODER_RESP_FLAG_BLOCK_GET;
    double fps = st_frame_rate(req->fps);
    if (!fps) fps = 60;
    sleep_time_us = static_cast<int>(1000 * 1000 / fps * 8 / 10);
  }

  static void set_block_timeout(SessionHandle s, uint64_t ns) {
    st22_encoder_set_block_timeout(s, ns);
  }

  static void wake_block(SessionHandle s) {
    st22_encoder_wake_block(s);
  }

  static FrameMeta* get_frame(SessionHandle s) {
    return st22_encoder_get_frame(s);
  }

  static int put_frame(SessionHandle s, FrameMeta* f, int result) {
    return st22_encoder_put_frame(s, f, result);
  }

  static int process_frame(const CreateReq& req, FrameMeta* f, int sleep_time_us,
                           int rand_ratio, int& frame_cnt) {
    size_t codestream_size = req.max_codestream_size;
    if (f->src->width != req.width || f->dst->width != req.width ||
        f->src->height != req.height || f->dst->height != req.height ||
        f->src->fmt != req.input_fmt || f->dst->fmt != req.output_fmt) {
      return -EIO;
    }

    /* copy src sha to the start of encode frame */
    memcpy(
        f->dst->addr[0],
        static_cast<uint8_t*>(f->src->addr[0]) + f->src->data_size - SHA256_DIGEST_LENGTH,
        SHA256_DIGEST_LENGTH);
    if (sleep_time_us > 0) st_usleep(static_cast<unsigned int>(sleep_time_us));
    if (rand_ratio > 0) {
      int ratio =
          100 - static_cast<int>(next_prng_u64() % static_cast<uint64_t>(rand_ratio));
      codestream_size = codestream_size * static_cast<size_t>(ratio) / 100;
    }
    f->dst->data_size = codestream_size;
    frame_cnt++;
    return 0;
  }
};

struct St22DecoderTraits {
  using SessionHandle = st22p_decode_session;
  using PrivHandle = st22_decode_priv;
  using CreateReq = struct st22_decoder_create_req;
  using FrameMeta = struct st22_decode_frame_meta;

  static bool use_block_get(struct st_tests_context* ctx) {
    return ctx->decoder_use_block_get;
  }

  static void on_create_req(struct st_tests_context* ctx, CreateReq* req,
                            int& sleep_time_us) {
    if (ctx->decoder_use_block_get) req->resp_flag |= ST22_DECODER_RESP_FLAG_BLOCK_GET;
    double fps = st_frame_rate(req->fps);
    if (!fps) fps = 60;
    sleep_time_us = static_cast<int>(1000 * 1000 / fps * 8 / 10);
  }

  static void set_block_timeout(SessionHandle s, uint64_t ns) {
    st22_decoder_set_block_timeout(s, ns);
  }

  static void wake_block(SessionHandle s) {
    st22_decoder_wake_block(s);
  }

  static FrameMeta* get_frame(SessionHandle s) {
    return st22_decoder_get_frame(s);
  }

  static int put_frame(SessionHandle s, FrameMeta* f, int result) {
    return st22_decoder_put_frame(s, f, result);
  }

  static int process_frame(const CreateReq& req, FrameMeta* f, int sleep_time_us,
                           int /*rand_ratio*/, int& frame_cnt) {
    if (f->src->width != req.width || f->dst->width != req.width ||
        f->src->height != req.height || f->dst->height != req.height ||
        f->src->fmt != req.input_fmt || f->dst->fmt != req.output_fmt ||
        f->src->data_size > f->src->buffer_size) {
      return -EIO;
    }

    /* copy sha to the end of decode frame */
    memcpy(
        static_cast<uint8_t*>(f->dst->addr[0]) + f->dst->data_size - SHA256_DIGEST_LENGTH,
        f->src->addr[0], SHA256_DIGEST_LENGTH);
    if (sleep_time_us > 0) st_usleep(static_cast<unsigned int>(sleep_time_us));
    frame_cnt++;
    return 0;
  }
};

using St22MockEncoder = mtl_test::MockPluginPool<St22EncoderTraits>;
using St22MockDecoder = mtl_test::MockPluginPool<St22DecoderTraits>;

}  // namespace

int st_test_st22_plugin_unregister(struct st_tests_context* ctx) {
  if (ctx->decoder_dev_handle) {
    st22_decoder_unregister(ctx->decoder_dev_handle);
    ctx->decoder_dev_handle = NULL;
  }
  if (ctx->encoder_dev_handle) {
    st22_encoder_unregister(ctx->encoder_dev_handle);
    ctx->encoder_dev_handle = NULL;
  }
  return 0;
}

int st_test_st22_plugin_register(struct st_tests_context* ctx) {
  auto st = ctx->handle;

  struct st22_decoder_dev d_dev = {};
  d_dev.name = "st22_test_decoder";
  d_dev.priv = ctx;
  d_dev.target_device = ST_PLUGIN_DEVICE_TEST;
  d_dev.input_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM | ST_FMT_CAP_H264_CBR_CODESTREAM;
  d_dev.output_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422PLANAR8;
  d_dev.create_session = St22MockDecoder::create_session;
  d_dev.free_session = St22MockDecoder::free_session;
  d_dev.notify_frame_available = St22MockDecoder::notify_frame_available;
  ctx->decoder_dev_handle = st22_decoder_register(st, &d_dev);
  if (!ctx->decoder_dev_handle) {
    err("%s, decoder register fail\n", __func__);
    return -EIO;
  }

  struct st22_encoder_dev e_dev = {};
  e_dev.name = "st22_test_encoder";
  e_dev.priv = ctx;
  e_dev.target_device = ST_PLUGIN_DEVICE_TEST;
  e_dev.input_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422PLANAR8;
  e_dev.output_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM | ST_FMT_CAP_H264_CBR_CODESTREAM;
  e_dev.create_session = St22MockEncoder::create_session;
  e_dev.free_session = St22MockEncoder::free_session;
  e_dev.notify_frame_available = St22MockEncoder::notify_frame_available;
  ctx->encoder_dev_handle = st22_encoder_register(st, &e_dev);
  if (!ctx->encoder_dev_handle) {
    err("%s, encoder register fail\n", __func__);
    st_test_st22_plugin_unregister(ctx);
    return -EIO;
  }

  info("%s, succ\n", __func__);
  return 0;
}

template <typename Dev, typename ScopedDev, typename CreateFn, typename FreeFn,
          typename RegFn>
void verify_codec_plugin_reg(mtl_handle st, Dev dev, CreateFn noop_create,
                             FreeFn noop_free, RegFn reg_fn) {
  dev.create_session = nullptr;
  dev.free_session = noop_free;
  EXPECT_EQ(reg_fn(st, &dev), nullptr);

  dev.create_session = noop_create;
  dev.free_session = nullptr;
  EXPECT_EQ(reg_fn(st, &dev), nullptr);

  dev.free_session = noop_free;
  ScopedDev handle(reg_fn(st, &dev));
  ASSERT_TRUE(static_cast<bool>(handle));
  handle.reset();

  handle.reset(reg_fn(st, &dev));
  ASSERT_TRUE(static_cast<bool>(handle));
}

TEST(St22p, codec_plugin_register_unregister) {
  auto st = st_test_ctx()->handle;

  struct st22_encoder_dev edev = {};
  edev.name = "st22_test_enc_reg";
  edev.target_device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  edev.input_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE;
  edev.output_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM;
  verify_codec_plugin_reg<struct st22_encoder_dev, ScopedSt22EncoderDev>(
      st, edev,
      +[](void*, st22p_encode_session,
          struct st22_encoder_create_req*) -> st22_encode_priv { return nullptr; },
      +[](void*, st22_encode_priv) -> int { return 0; }, st22_encoder_register);

  struct st22_decoder_dev ddev = {};
  ddev.name = "st22_test_dec_reg";
  ddev.target_device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  ddev.input_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM;
  ddev.output_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE;
  verify_codec_plugin_reg<struct st22_decoder_dev, ScopedSt22DecoderDev>(
      st, ddev,
      +[](void*, st22p_decode_session,
          struct st22_decoder_create_req*) -> st22_decode_priv { return nullptr; },
      +[](void*, st22_decode_priv) -> int { return 0; }, st22_decoder_register);
}

static int test_st22p_frame_available(void* priv) {
  auto* s = static_cast<tests_context*>(priv);
  std::lock_guard<std::mutex> lck(s->mtx);
  s->cv.notify_all();
  return 0;
}

static int test_st22p_tx_frame_done(void* priv, struct st_frame* frame) {
  auto* s = static_cast<St22pContext*>(priv);
  if (!get_handle(s)) return -EIO;

  atomic_inc(&s->fb_send_done);
  if (!(frame->flags & ST_FRAME_FLAG_EXT_BUF)) return 0;

  const size_t n = s->ext_frames.count();
  for (size_t i = 0; i < n; ++i) {
    if (frame->addr[0] == s->ext_frames.frames()[i].addr[0]) {
      s->ext_frames.release_slot(i);
      dbg("%s(%zu), frame done at %d\n", __func__, i, s->idx);
      return 0;
    }
  }

  err("%s(%d), unknown frame_addr %p\n", __func__, s->idx, frame->addr[0]);
  return 0;
}

static int test_st22p_rx_query_ext_frame(void* priv, st_ext_frame* ext_frame,
                                         struct st22_rx_frame_meta* /*meta*/) {
  auto* s = static_cast<St22pContext*>(priv);
  if (!get_handle(s)) return -EIO;

  const size_t n = s->ext_frames.count();
  if (n == 0) return -EIO;

  const size_t start = static_cast<size_t>(s->ext_idx) % n;
  for (size_t attempt = 0; attempt < n; ++attempt) {
    const size_t i = (start + attempt) % n;
    if (!s->ext_frames.try_acquire_slot(i)) continue;

    *ext_frame = s->ext_frames.frames()[i];
    ext_frame->opaque = s->ext_frames.in_use_slot(i);
    dbg("%s(%d), set ext frame %zu(%p) to use\n", __func__, s->idx, i,
        ext_frame->addr[0]);
    s->ext_idx = static_cast<int>((i + 1) % n);
    return 0;
  }

  err("%s(%d), all %zu ext frames in use\n", __func__, s->idx, n);
  return -EIO;
}

static void st22p_tx_ops_init(tests_context* st22, struct st22p_tx_ops* ops_tx) {
  auto ctx = st22->ctx;

  memset(ops_tx, 0, sizeof(*ops_tx));
  ops_tx->name = "st22p_test";
  ops_tx->priv = st22;
  ops_tx->port.num_port = 1;
  memcpy(ops_tx->port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx->port.udp_port[MTL_SESSION_PORT_P] = ST22P_TEST_UDP_PORT + st22->idx;
  ops_tx->port.payload_type = ST22P_TEST_PAYLOAD_TYPE;
  ops_tx->width = 1920;
  ops_tx->height = 1080;
  ops_tx->fps = ST_FPS_P59_94;
  ops_tx->input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  ops_tx->pack_type = ST22_PACK_CODESTREAM;
  ops_tx->codec = ST22_CODEC_JPEGXS;
  ops_tx->device = ST_PLUGIN_DEVICE_TEST;
  ops_tx->quality = ST22_QUALITY_MODE_QUALITY;
  ops_tx->framebuff_cnt = st22->fb_cnt;
  ops_tx->notify_frame_available = test_st22p_frame_available;
  st22->frame_size =
      st_frame_size(ops_tx->input_fmt, ops_tx->width, ops_tx->height, false);
  ops_tx->codestream_size = st22->frame_size / 8;
  ops_tx->notify_event = test_ctx_notify_event;
}

static void st22p_rx_ops_init(tests_context* st22, struct st22p_rx_ops* ops_rx) {
  auto ctx = st22->ctx;

  memset(ops_rx, 0, sizeof(*ops_rx));
  ops_rx->name = "st22p_test";
  ops_rx->priv = st22;
  ops_rx->port.num_port = 1;
  memcpy(ops_rx->port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_rx->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops_rx->port.udp_port[MTL_SESSION_PORT_P] = ST22P_TEST_UDP_PORT + st22->idx;
  ops_rx->port.payload_type = ST22P_TEST_PAYLOAD_TYPE;
  ops_rx->width = 1920;
  ops_rx->height = 1080;
  ops_rx->fps = ST_FPS_P59_94;
  ops_rx->output_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  ops_rx->pack_type = ST22_PACK_CODESTREAM;
  ops_rx->codec = ST22_CODEC_JPEGXS;
  ops_rx->device = ST_PLUGIN_DEVICE_TEST;
  ops_rx->framebuff_cnt = st22->fb_cnt;
  ops_rx->notify_frame_available = test_st22p_frame_available;
  st22->frame_size =
      st_frame_size(ops_rx->output_fmt, ops_rx->width, ops_rx->height, false);
  ops_rx->notify_event = test_ctx_notify_event;
}

static void st22p_tx_assert_cnt(int expect_s22_tx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st22_tx_sessions_cnt, expect_s22_tx_cnt);
}

static void st22p_rx_assert_cnt(int expect_s22_rx_cnt) {
  auto ctx = st_test_ctx();
  struct st_var_info var;
  EXPECT_GE(st_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.st22_rx_sessions_cnt, expect_s22_rx_cnt);
}

TEST(St22p, tx_create_free_mix) {
  mtl_test::verify_create_free<struct st22p_tx_ops, st22p_tx_handle, st22p_tx_create,
                               st22p_tx_free>(st22p_tx_ops_init, st22p_tx_assert_cnt, 2,
                                              3, 4);
}
TEST(St22p, rx_create_free_mix) {
  mtl_test::verify_create_free<struct st22p_rx_ops, st22p_rx_handle, st22p_rx_create,
                               st22p_rx_free>(st22p_rx_ops_init, st22p_rx_assert_cnt, 2,
                                              3, 4);
}
TEST(St22p, tx_create_free_max) {
  mtl_test::verify_create_free_max<struct st22p_tx_ops, st22p_tx_handle, st22p_tx_create,
                                   st22p_tx_free>(st22p_tx_ops_init, st22p_tx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}
TEST(St22p, rx_create_free_max) {
  mtl_test::verify_create_free_max<struct st22p_rx_ops, st22p_rx_handle, st22p_rx_create,
                                   st22p_rx_free>(st22p_rx_ops_init, st22p_rx_assert_cnt,
                                                  TEST_CREATE_FREE_MAX);
}
TEST(St22p, tx_create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st22p_tx_ops, st22p_tx_handle, st22p_tx_create,
                                 st22p_tx_free>(st22p_tx_ops_init);
}
TEST(St22p, rx_create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st22p_rx_ops, st22p_rx_handle, st22p_rx_create,
                                 st22p_rx_free>(st22p_rx_ops_init);
}
TEST(St22p, tx_create_expect_fail_fb_cnt) {
  mtl_test::verify_invalid_fb_cnt<struct st22p_tx_ops, st22p_tx_handle, st22p_tx_create,
                                  st22p_tx_free>(st22p_tx_ops_init, 1);
  mtl_test::verify_invalid_fb_cnt<struct st22p_tx_ops, st22p_tx_handle, st22p_tx_create,
                                  st22p_tx_free>(st22p_tx_ops_init,
                                                 ST22_FB_MAX_COUNT + 1);
}
TEST(St22p, rx_create_expect_fail_fb_cnt) {
  mtl_test::verify_invalid_fb_cnt<struct st22p_rx_ops, st22p_rx_handle, st22p_rx_create,
                                  st22p_rx_free>(st22p_rx_ops_init, 1);
  mtl_test::verify_invalid_fb_cnt<struct st22p_rx_ops, st22p_rx_handle, st22p_rx_create,
                                  st22p_rx_free>(st22p_rx_ops_init,
                                                 ST22_FB_MAX_COUNT + 1);
}
TEST(St22p, tx_query_params) {
  auto ctx = st_test_ctx();
  constexpr uint16_t kFbCnt = 3;
  auto test_ctx = mtl_test::make_context(ctx, 0, kFbCnt);

  struct st22p_tx_ops ops;
  st22p_tx_ops_init(test_ctx.get(), &ops);
  ScopedSt22pTx handle(st22p_tx_create(ctx->handle, &ops));
  ASSERT_TRUE(static_cast<bool>(handle));

  EXPECT_EQ(st22p_tx_frame_size(handle.get()), test_ctx->frame_size);
  for (uint16_t idx = 0; idx < kFbCnt; idx++) {
    EXPECT_NE(st22p_tx_get_fb_addr(handle.get(), idx), nullptr);
  }
  EXPECT_EQ(st22p_tx_get_fb_addr(handle.get(), kFbCnt), nullptr);
  EXPECT_EQ(st22p_tx_get_fb_addr(handle.get(), kFbCnt * 2), nullptr);
}
TEST(St22p, rx_query_params) {
  auto ctx = st_test_ctx();
  constexpr uint16_t kFbCnt = 3;
  auto test_ctx = mtl_test::make_context(ctx, 0, kFbCnt);

  struct st22p_rx_ops ops;
  st22p_rx_ops_init(test_ctx.get(), &ops);
  ScopedSt22pRx handle(st22p_rx_create(ctx->handle, &ops));
  ASSERT_TRUE(static_cast<bool>(handle));

  EXPECT_EQ(st22p_rx_frame_size(handle.get()), test_ctx->frame_size);
  for (uint16_t idx = 0; idx < kFbCnt; idx++) {
    EXPECT_NE(st22p_rx_get_fb_addr(handle.get(), idx), nullptr);
  }
  EXPECT_EQ(st22p_rx_get_fb_addr(handle.get(), kFbCnt), nullptr);
  EXPECT_EQ(st22p_rx_get_fb_addr(handle.get(), kFbCnt * 2), nullptr);
}

template <typename GetFn>
static struct st_frame* st22p_get_frame_or_wait(St22pContext* s, GetFn&& get_fn) {
  struct st_frame* frame = get_fn();
  if (frame || s->block_get) return frame;
  std::unique_lock<std::mutex> lck(s->mtx);
  frame = get_fn();
  if (!frame && !s->stop) s->cv.wait(lck);
  return frame;
}

static void populate_st22p_tx_sha(St22pContext* ctx, st22p_tx_handle handle, bool tx_ext,
                                  const St22pContext* copy_sha_from = nullptr) {
  const size_t frame_size = ctx->frame_size;
  for (int frame = 0; frame < ST22_TEST_SHA_HIST_NUM; frame++) {
    uint8_t* fb = tx_ext ? (ctx->ext_frames.data() + frame * frame_size)
                         : (uint8_t*)st22p_tx_get_fb_addr(handle, frame);
    ASSERT_NE(fb, nullptr);
    st_test_rand_data(fb, frame_size, frame);
    if (copy_sha_from) {
      memcpy(ctx->shas[frame], copy_sha_from->shas[frame], SHA256_DIGEST_LENGTH);
    } else {
      SHA256((unsigned char*)fb, frame_size, ctx->shas[frame]);
      test_sha_dump("st22p_tx", ctx->shas[frame]);
    }
    memcpy(fb + frame_size - SHA256_DIGEST_LENGTH, ctx->shas[frame],
           SHA256_DIGEST_LENGTH);
  }
}

static void test_st22p_tx_frame_thread(St22pContext* s) {
  auto handle = static_cast<st22p_tx_handle>(get_handle(s));

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    struct st_frame* frame =
        st22p_get_frame_or_wait(s, [handle]() { return st22p_tx_get_frame(handle); });
    if (!frame) continue;
    if (frame->data_size != s->frame_size || frame->buffer_size != s->frame_size ||
        frame->width != s->width || frame->height != s->height || frame->fmt != s->fmt) {
      atomic_inc(&s->incomplete_frame_cnt);
    }
    if (s->user_timestamp) {
      const int sent = mtl_test::tx_frames(s);
      frame->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
      frame->timestamp = sent + 1; /* add one to avoid zero timestamp */
      dbg("%s(%d), timestamp %d\n", __func__, s->idx, sent);
    }
    if (s->ext_frames.count() > 0) {
      const size_t n = s->ext_frames.count();
      size_t slot = static_cast<size_t>(s->ext_idx) % n;
      bool acquired = false;
      while (!s->stop) {
        for (size_t attempt = 0; attempt < n; ++attempt) {
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
        st22p_tx_put_frame_abort(handle, frame);
        break;
      }
      int ret = st22p_tx_put_ext_frame(handle, frame, &s->ext_frames.frames()[slot]);
      if (ret < 0) {
        s->ext_frames.release_slot(slot);
        st22p_tx_put_frame_abort(handle, frame);
        err("%s, put ext framebuffer fail %d fb_idx %zu\n", __func__, ret, slot);
        continue;
      }
      s->ext_idx = static_cast<int>((slot + 1) % n);
    } else {
      st22p_tx_put_frame(handle, frame);
    }
    atomic_inc(&s->fb_send);
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void test_st22p_rx_frame_thread(St22pContext* s) {
  auto handle = static_cast<st22p_rx_handle>(get_handle(s));

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    struct st_frame* frame =
        st22p_get_frame_or_wait(s, [handle]() { return st22p_rx_get_frame(handle); });
    if (!frame) continue;

    auto* in_use = static_cast<std::atomic<bool>*>(frame->opaque);

    if (frame->data_size != s->frame_size || frame->buffer_size < s->frame_size ||
        frame->width != s->width || frame->height != s->height || frame->fmt != s->fmt ||
        frame->timestamp == 0) {
      atomic_inc(&s->incomplete_frame_cnt);
    }
    dbg("%s(%d), timestamp %" PRIu64 "\n", __func__, s->idx, frame->timestamp);

    if (s->user_timestamp && !s->user_pacing) {
      if (s->pre_timestamp && ((uint32_t)frame->timestamp - s->pre_timestamp) > 4) {
        atomic_inc(&s->incomplete_frame_cnt);
        err("%s(%d), frame user timestamp %" PRIu64 " pre_timestamp %u\n", __func__,
            s->idx, frame->timestamp, s->pre_timestamp);
      }
      s->pre_timestamp = (uint32_t)frame->timestamp;
    }

    unsigned char* sha =
        (unsigned char*)frame->addr[0] + frame->data_size - SHA256_DIGEST_LENGTH;
    int i = 0;
    for (i = 0; i < ST22_TEST_SHA_HIST_NUM; i++) {
      if (!memcmp(sha, s->shas[i], SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= ST22_TEST_SHA_HIST_NUM) {
      test_sha_dump("st22p_rx_error_sha", sha);
      atomic_inc(&s->sha_fail_cnt);
    }
    st22p_rx_put_frame(handle, frame);
    if (in_use && !in_use->exchange(false, std::memory_order_acq_rel)) {
      atomic_inc(&s->incomplete_frame_cnt);
    }
    atomic_inc(&s->fb_rec);
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void st22p_rx_digest_test(const St22pDigestScenario& sc) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  /* All GTEST_SKIP() precondition checks MUST precede plugin state mutation. */
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  if (sc.tx_ext || sc.rx_ext) MTL_TEST_REQUIRE_IOVA_VA(ctx);

  mtl_test::ScopedPluginFaults plugin_cfg(ctx, sc.fail_interval, sc.timeout_interval,
                                          sc.timeout_ms, sc.rand_ratio,
                                          sc.codec_block_get);

  const size_t sessions = sc.streams.size();
  std::vector<St22pTxSession> tx;
  std::vector<St22pRxSession> rx;
  mtl_test::ScopedMtlStart mtl_guard(st);
  std::vector<mtl_test::ScopedWorker> workers;
  std::vector<double> expect_framerate_tx(sessions);
  std::vector<double> expect_framerate_rx(sessions);
  tx.reserve(sessions);
  rx.reserve(sessions);

  for (size_t i = 0; i < sessions; i++) {
    const auto& stream = sc.streams[i];
    const uint16_t udp_port = ST22P_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

    expect_framerate_tx[i] = st_frame_rate(stream.fps);
    if (sc.timeout_interval) {
      expect_framerate_tx[i] =
          expect_framerate_tx[i] * (sc.timeout_interval - 1) / sc.timeout_interval;
    }

    tx.emplace_back();
    auto& s = tx.back();
    s.ctx = make_st22p_context(ctx, static_cast<int>(i), ST22_TEST_SHA_HIST_NUM);
    s.ctx->width = stream.width;
    s.ctx->height = stream.height;
    s.ctx->fmt = stream.fmt;
    s.ctx->user_timestamp = sc.user_timestamp;
    s.ctx->block_get = sc.block_get;

    struct st22p_tx_ops ops_tx;
    st22p_tx_ops_init(s.ctx.get(), &ops_tx);
    mtl_test::fill_tx_port(ops_tx.port, ctx, udp_port, ST22P_TEST_PAYLOAD_TYPE, sc.ssrc);
    ops_tx.width = stream.width;
    ops_tx.height = stream.height;
    ops_tx.fps = stream.fps;
    ops_tx.interlaced = sc.interlace;
    ops_tx.input_fmt = stream.fmt;
    ops_tx.codec = stream.codec;
    ops_tx.notify_frame_done = test_st22p_tx_frame_done;
    if (sc.block_get) {
      ops_tx.notify_frame_available = nullptr;
      ops_tx.flags |= ST22P_TX_FLAG_BLOCK_GET;
    }
    if (sc.user_timestamp) ops_tx.flags |= ST22P_TX_FLAG_USER_TIMESTAMP;
    if (sc.vsync) ops_tx.flags |= ST22P_TX_FLAG_ENABLE_VSYNC;
    if (sc.tx_ext) ops_tx.flags |= ST22P_TX_FLAG_EXT_FRAME;
    if (sc.rtcp) {
      ops_tx.flags |= ST22P_TX_FLAG_ENABLE_RTCP;
      ops_tx.rtcp.buffer_size = 512;
    }

    if (sc.derive) {
      ops_tx.codestream_size = ops_tx.width * ops_tx.height / stream.compress_ratio;
      s.ctx->frame_size = ops_tx.codestream_size;
    } else {
      s.ctx->frame_size =
          st_frame_size(ops_tx.input_fmt, ops_tx.width, ops_tx.height, ops_tx.interlaced);
      ops_tx.codestream_size = s.ctx->frame_size / stream.compress_ratio;
    }

    s.handle.reset(st22p_tx_create(st, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    EXPECT_EQ(s.ctx->frame_size, st22p_tx_frame_size(s.handle.get()));
    if (sc.block_get) {
      EXPECT_EQ(st22p_tx_set_block_timeout(s.handle.get(), NS_PER_S), 0);
    }

    if (sc.tx_ext) {
      ASSERT_TRUE(s.ctx->ext_frames.init(st, stream.fmt, stream.width, stream.height,
                                         sc.interlace, s.ctx->fb_cnt));
      info("%s, session %zu tx ext_frames %p\n", __func__, i, s.ctx->ext_frames.data());
    }

    populate_st22p_tx_sha(s.ctx.get(), s.handle.get(), sc.tx_ext);
    if (::testing::Test::HasFatalFailure()) return;

    set_handle(s.ctx.get(), s.handle.get());
    st22p_tx_handle raw_h = s.handle.get();
    workers.emplace_back(
        s.ctx.get(), std::thread(test_st22p_tx_frame_thread, s.ctx.get()),
        sc.block_get ? std::function<void()>([raw_h]() { st22p_tx_wake_block(raw_h); })
                     : nullptr);
  }

  for (size_t i = 0; i < sessions; i++) {
    const auto& stream = sc.streams[i];
    const uint16_t udp_port = ST22P_TEST_UDP_PORT + static_cast<uint16_t>(i * 2);

    if (sc.fail_interval) {
      expect_framerate_tx[i] =
          expect_framerate_tx[i] * (sc.fail_interval - 1) / sc.fail_interval;
    }
    expect_framerate_rx[i] = expect_framerate_tx[i];

    rx.emplace_back();
    auto& s = rx.back();
    s.ctx = make_st22p_context(ctx, static_cast<int>(i), ST22_TEST_SHA_HIST_NUM);
    s.ctx->width = stream.width;
    s.ctx->height = stream.height;
    s.ctx->fmt = stream.fmt;
    s.ctx->user_timestamp = sc.user_timestamp;
    s.ctx->block_get = sc.block_get;
    memcpy(s.ctx->shas, tx[i].ctx->shas, ST22_TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    if (sc.rx_ext) {
      ASSERT_TRUE(s.ctx->ext_frames.init(st, stream.fmt, stream.width, stream.height,
                                         sc.interlace, s.ctx->fb_cnt));
      info("%s, session %zu rx ext_frames %p\n", __func__, i, s.ctx->ext_frames.data());
    }

    struct st22p_rx_ops ops_rx;
    st22p_rx_ops_init(s.ctx.get(), &ops_rx);
    mtl_test::fill_rx_port(ops_rx.port, ctx, udp_port, ST22P_TEST_PAYLOAD_TYPE, sc.ssrc);
    ops_rx.width = stream.width;
    ops_rx.height = stream.height;
    ops_rx.fps = stream.fps;
    ops_rx.interlaced = sc.interlace;
    ops_rx.output_fmt = stream.fmt;
    ops_rx.codec = stream.codec;
    if (sc.block_get) {
      ops_rx.notify_frame_available = nullptr;
      ops_rx.flags |= ST22P_RX_FLAG_BLOCK_GET;
    }
    if (sc.vsync) ops_rx.flags |= ST22P_RX_FLAG_ENABLE_VSYNC;
    if (sc.rx_ext) {
      ops_rx.flags |= ST22P_RX_FLAG_EXT_FRAME;
      ops_rx.query_ext_frame = test_st22p_rx_query_ext_frame;
    }
    if (sc.rtcp) {
      ops_rx.flags |= ST22P_RX_FLAG_ENABLE_RTCP | ST22P_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 100;
      ops_rx.rtcp.seq_skip_window = 0;
      ops_rx.rtcp.burst_loss_max = 4;
      ops_rx.rtcp.sim_loss_rate = 0.0001;
    }

    if (sc.derive) {
      s.ctx->frame_size = tx[i].ctx->frame_size;
      ops_rx.max_codestream_size = s.ctx->frame_size;
    } else {
      s.ctx->frame_size = st_frame_size(ops_rx.output_fmt, ops_rx.width, ops_rx.height,
                                        ops_rx.interlaced);
    }

    s.handle.reset(st22p_rx_create(st, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    EXPECT_EQ(s.ctx->frame_size, st22p_rx_frame_size(s.handle.get()));
    if (sc.block_get) {
      EXPECT_EQ(st22p_rx_set_block_timeout(s.handle.get(), NS_PER_S), 0);
    }

    set_handle(s.ctx.get(), s.handle.get());
    st22p_rx_handle raw_h = s.handle.get();
    workers.emplace_back(
        s.ctx.get(), std::thread(test_st22p_rx_frame_thread, s.ctx.get()),
        sc.block_get ? std::function<void()>([raw_h]() { st22p_rx_wake_block(raw_h); })
                     : nullptr);

    struct st_queue_meta meta = {};
    EXPECT_GE(st22p_rx_get_queue_meta(s.handle.get(), &meta), 0);
    EXPECT_EQ(meta.num_port, 1);
  }

  EXPECT_GE(mtl_guard.start(), 0);

  /* Capture steady-state FPS and vsync rates BEFORE teardown (--auto_start_stop safe). */
  auto tx_ctxs = collect_ctxs(tx);
  auto rx_ctxs = collect_ctxs(rx);
  auto win =
      mtl_test::measure_pipeline_window(tx_ctxs, rx_ctxs, kSt22pRateWarmupFrames,
                                        kSt22pRateMinDeltaFrames, kSt22pRateMinWindowMs);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);

  for (auto& w : workers) w.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const double nominal_fps = st_frame_rate(sc.streams[i].fps);
    const int tx_vsync = mtl_test::vsync_count(tx[i].ctx.get());
    const int rx_vsync = mtl_test::vsync_count(rx[i].ctx.get());
    const int tx_fb_send = mtl_test::tx_frames(tx[i].ctx.get());
    const int rx_fb_rec = mtl_test::rx_frames(rx[i].ctx.get());

    if (sc.vsync) {
      EXPECT_GT(tx_vsync, 0);
      EXPECT_NEAR(win.tx_vsync[i], nominal_fps, nominal_fps * 0.1);
      EXPECT_GT(rx_vsync, 0);
      EXPECT_NEAR(win.rx_vsync[i], nominal_fps, nominal_fps * 0.1);
    }

    info("%s, session %zu fb_send %d framerate %f:%f\n", __func__, i, tx_fb_send,
         win.tx_fps[i], expect_framerate_tx[i]);
    info("%s, session %zu fb_rec %d framerate %f:%f\n", __func__, i, rx_fb_rec,
         win.rx_fps[i], expect_framerate_rx[i]);
    EXPECT_GT(tx_fb_send, 0);
    EXPECT_EQ(mtl_test::relaxed_load(&tx[i].ctx->incomplete_frame_cnt), 0);
    EXPECT_GT(rx_fb_rec, 0);
    EXPECT_EQ(mtl_test::relaxed_load(&rx[i].ctx->incomplete_frame_cnt), 0);
    EXPECT_EQ(mtl_test::relaxed_load(&rx[i].ctx->sha_fail_cnt), 0);
    if (sc.check_fps) {
      const double tol = (sc.fail_interval || sc.timeout_interval) ? 0.5 : 0.1;
      EXPECT_NEAR(win.rx_fps[i], expect_framerate_rx[i], expect_framerate_rx[i] * tol);
    }
  }
}

TEST(St22p, digest_st22_1080i) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 10},
  };
  sc.interlace = true;
  sc.block_get = true;
  st22p_rx_digest_test(sc);
}

TEST(St22p, digest_st22_s2) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920 * 2, 1080 * 2, ST_FRAME_FMT_YUV422PLANAR10LE,
       ST22_CODEC_JPEGXS, 20},
      {ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 10},
  };
  sc.user_timestamp = true;
  sc.ssrc = 778899;
  sc.codec_block_get = true;
  st22p_rx_digest_test(sc);
}

TEST(St22p, digest_st22_1080p_fail_interval) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 10},
  };
  sc.fail_interval = 3;
  sc.block_get = true;
  st22p_rx_digest_test(sc);
}

TEST(St22p, digest_st22_1080p_timeout_interval) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 10},
  };
  sc.timeout_interval = 3;
  sc.timeout_ms = 20;
  sc.codec_block_get = true;
  st22p_rx_digest_test(sc);
}

TEST(St22p, digest_st22_1080p_rand_size) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR8, ST22_CODEC_H264_CBR, 5},
  };
  sc.rand_ratio = 30;
  st22p_rx_digest_test(sc);
}

TEST(St22p, digest_st22_s2_rtcp) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 10},
      {ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 16},
  };
  sc.check_fps = false;
  sc.rtcp = true;
  st22p_rx_digest_test(sc);
}

TEST(St22p, digest_st22_s2_ext) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 10},
      {ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 16},
  };
  sc.tx_ext = true;
  sc.rx_ext = true;
  sc.codec_block_get = true;
  st22p_rx_digest_test(sc);
}

TEST(St22p, digest_derive_s2) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_JPEGXS_CODESTREAM, ST22_CODEC_JPEGXS, 5},
      {ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_H264_CBR_CODESTREAM, ST22_CODEC_H264_CBR, 8},
  };
  sc.derive = true;
  st22p_rx_digest_test(sc);
}

namespace {

/* Compact RAII ST22p loopback fixture reused by update_src_dst, pcapng, and abort tests.
 */
struct St22pLoopback {
  struct Opts {
    int tx_sessions = 1;
    uint16_t fb_cnt = ST22_TEST_SHA_HIST_NUM;
    bool block_get = true;
    bool populate_sha = false;
    bool custom_mcast_tx = false;
    bool start_tx_workers = true;
    bool start_rx_worker = true;
    bool start_mtl = true;
  };

  std::vector<St22pTxSession> tx;
  std::vector<St22pRxSession> rx;
  mtl_test::ScopedMtlStart mtl_guard;
  std::vector<mtl_test::ScopedWorker> workers;

  explicit St22pLoopback(mtl_handle st) : mtl_guard(st) {
  }

  void init(struct st_tests_context* ctx, const Opts& opts) {
    auto st = ctx->handle;
    const St22pStreamSpec stream;
    tx.reserve(static_cast<size_t>(opts.tx_sessions));
    rx.reserve(1);

    for (int i = 0; i < opts.tx_sessions; i++) {
      tx.emplace_back();
      auto& s = tx.back();
      s.ctx = make_st22p_context(ctx, i, opts.fb_cnt);
      s.ctx->width = stream.width;
      s.ctx->height = stream.height;
      s.ctx->fmt = stream.fmt;
      s.ctx->block_get = opts.block_get;

      struct st22p_tx_ops ops_tx;
      st22p_tx_ops_init(s.ctx.get(), &ops_tx);
      mtl_test::fill_tx_port(ops_tx.port, ctx,
                             ST22P_TEST_UDP_PORT + static_cast<uint16_t>(i * 2),
                             ST22P_TEST_PAYLOAD_TYPE);
      if (opts.custom_mcast_tx) {
        if (i == 1) {
          memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
                 MTL_IP_ADDR_LEN);
        } else if (ctx->mcast_only) {
          memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
                 MTL_IP_ADDR_LEN);
        }
      }
      ops_tx.codestream_size = s.ctx->frame_size / stream.compress_ratio;
      if (opts.block_get) {
        ops_tx.notify_frame_available = nullptr;
        ops_tx.notify_event = nullptr;
        ops_tx.flags |= ST22P_TX_FLAG_BLOCK_GET;
      }

      s.handle.reset(st22p_tx_create(st, &ops_tx));
      ASSERT_TRUE(static_cast<bool>(s.handle));
      if (opts.block_get) {
        EXPECT_EQ(st22p_tx_set_block_timeout(s.handle.get(), NS_PER_S), 0);
      }

      if (opts.populate_sha) {
        populate_st22p_tx_sha(s.ctx.get(), s.handle.get(), false,
                              i == 0 ? nullptr : tx[0].ctx.get());
        if (::testing::Test::HasFatalFailure()) return;
      }
      set_handle(s.ctx.get(), s.handle.get());
      if (opts.start_tx_workers) start_tx_worker(static_cast<size_t>(i), opts.block_get);
    }

    rx.emplace_back();
    auto& r = rx.back();
    r.ctx = make_st22p_context(ctx, 0, opts.fb_cnt);
    r.ctx->width = stream.width;
    r.ctx->height = stream.height;
    r.ctx->fmt = stream.fmt;
    r.ctx->block_get = opts.block_get;
    if (opts.populate_sha) {
      memcpy(r.ctx->shas, tx[0].ctx->shas, ST22_TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    }

    struct st22p_rx_ops ops_rx;
    st22p_rx_ops_init(r.ctx.get(), &ops_rx);
    mtl_test::fill_rx_port(ops_rx.port, ctx, ST22P_TEST_UDP_PORT,
                           ST22P_TEST_PAYLOAD_TYPE);
    if (opts.custom_mcast_tx && ctx->mcast_only) {
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_2],
             MTL_IP_ADDR_LEN);
    }
    if (opts.block_get) {
      ops_rx.notify_frame_available = nullptr;
      ops_rx.notify_event = nullptr;
      ops_rx.flags |= ST22P_RX_FLAG_BLOCK_GET;
    }

    r.handle.reset(st22p_rx_create(st, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(r.handle));
    if (opts.block_get) {
      EXPECT_EQ(st22p_rx_set_block_timeout(r.handle.get(), NS_PER_S), 0);
    }
    set_handle(r.ctx.get(), r.handle.get());

    if (opts.start_rx_worker) {
      st22p_rx_handle raw_rx = r.handle.get();
      auto wake = opts.block_get
                      ? std::function<void()>([raw_rx]() { st22p_rx_wake_block(raw_rx); })
                      : nullptr;
      if (opts.populate_sha) {
        workers.emplace_back(r.ctx.get(),
                             std::thread(test_st22p_rx_frame_thread, r.ctx.get()), wake);
      } else {
        workers.emplace_back(r.ctx.get(),
                             std::thread(
                                 [](tests_context* s) {
                                   auto h = (st22p_rx_handle)get_handle(s);
                                   while (!s->stop) {
                                     auto* frame = st22p_rx_get_frame(h);
                                     if (!frame) continue;
                                     st22p_rx_put_frame(h, frame);
                                     atomic_inc(&s->fb_rec);
                                   }
                                 },
                                 r.ctx.get()),
                             wake);
      }
    }

    if (opts.start_mtl) {
      EXPECT_GE(mtl_guard.start(), 0);
    }
  }

  void start_tx_worker(size_t idx, bool block_get) {
    st22p_tx_handle raw_tx = tx[idx].handle.get();
    workers.emplace_back(
        tx[idx].ctx.get(), std::thread(test_st22p_tx_frame_thread, tx[idx].ctx.get()),
        block_get ? std::function<void()>([raw_tx]() { st22p_tx_wake_block(raw_tx); })
                  : nullptr);
  }

  void stop_all() {
    for (auto& w : workers) w.stop();
    EXPECT_GE(mtl_guard.stop(), 0);
  }
};

}  // namespace

static void st22p_update_src_dst_test(bool tx_update_dst) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  const double expect_fps = st_frame_rate(ST_FPS_P59_94);

  St22pLoopback lb(ctx->handle);
  St22pLoopback::Opts opts;
  opts.tx_sessions = tx_update_dst ? 1 : 2;
  opts.block_get = false;
  opts.populate_sha = true;
  opts.custom_mcast_tx = true;
  lb.init(ctx, opts);
  if (::testing::Test::HasFatalFailure()) return;

  auto rx_ctxs = collect_ctxs(lb.rx);
  EXPECT_TRUE(
      mtl_test::wait_for_all(rx_ctxs, mtl_test::rx_frames, kSt22pRateWarmupFrames, 6000));

  auto verify_switch = [&](const char* label, const uint8_t* tx_dst_ip,
                           const uint8_t* rx_src_ip, uint16_t udp_port) {
    if (tx_update_dst) {
      struct st_tx_dest_info dst = {};
      dst.udp_port[MTL_SESSION_PORT_P] = udp_port;
      memcpy(dst.dip_addr[MTL_SESSION_PORT_P], tx_dst_ip, MTL_IP_ADDR_LEN);
      EXPECT_GE(st22p_tx_update_destination(lb.tx[0].handle.get(), &dst), 0);
    }

    struct st_rx_source_info src = {};
    src.udp_port[MTL_SESSION_PORT_P] = udp_port;
    memcpy(src.ip_addr[MTL_SESSION_PORT_P], rx_src_ip, MTL_IP_ADDR_LEN);
    EXPECT_GE(st22p_rx_update_source(lb.rx[0].handle.get(), &src), 0);

    const int base_rec = mtl_test::rx_frames(lb.rx[0].ctx.get());
    auto delta_rec = [&](const tests_context* c) {
      return mtl_test::rx_frames(c) - base_rec;
    };
    auto rate_res =
        mtl_test::measure_rate_window(rx_ctxs, delta_rec, kSt22pRateWarmupFrames,
                                      kSt22pRateMinDeltaFrames, kSt22pRateMinWindowMs);
    EXPECT_TRUE(rate_res.warmed_up);
    EXPECT_TRUE(rate_res.complete);
    const int fb_rec = mtl_test::rx_frames(lb.rx[0].ctx.get()) - base_rec;
    EXPECT_GT(fb_rec, 0);
    info("%s, %s fb_rec %d framerate %f\n", __func__, label, fb_rec, rate_res.rate[0]);
    EXPECT_NEAR(rate_res.rate[0], expect_fps, expect_fps * 0.1);
  };

  verify_switch("mcast 1", ctx->mcast_ip_addr[MTL_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
                ST22P_TEST_UDP_PORT + 2);
  const uint8_t* tx0_dst_ip =
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_R];
  const uint8_t* rx0_src_ip =
      ctx->mcast_only ? ctx->mcast_ip_addr[MTL_PORT_2] : ctx->para.sip_addr[MTL_PORT_P];
  verify_switch("unicast 0", tx0_dst_ip, rx0_src_ip, ST22P_TEST_UDP_PORT);

  lb.stop_all();
  EXPECT_EQ(mtl_test::relaxed_load(&lb.rx[0].ctx->incomplete_frame_cnt), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&lb.rx[0].ctx->sha_fail_cnt), 0);
}

TEST(St22p, rx_update_source) {
  st22p_update_src_dst_test(/*tx_update_dst=*/false);
}

TEST(St22p, tx_update_destination) {
  st22p_update_src_dst_test(/*tx_update_dst=*/true);
}

TEST(St22p, rx_pcapng_dump) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  if (!mtl_pmd_is_dpdk_based(ctx->handle, MTL_PORT_R)) {
    GTEST_SKIP() << "MTL_PORT_R is not a DPDK-based PMD";
  }

  St22pLoopback lb(ctx->handle);
  St22pLoopback::Opts opts;
  lb.init(ctx, opts);
  if (::testing::Test::HasFatalFailure()) return;

  auto rx_ctxs = collect_ctxs(lb.rx);
  EXPECT_TRUE(
      mtl_test::wait_for_all(rx_ctxs, mtl_test::rx_frames, kSt22pRateWarmupFrames, 6000));

  constexpr uint32_t kMaxDumpPackets = 100;
  struct st_pcap_dump_meta meta = {};
  int ret = st22p_rx_pcapng_dump(lb.rx[0].handle.get(), kMaxDumpPackets, true, &meta);
  UnlinkGuard unlink_guard{meta.file_name[MTL_SESSION_PORT_P]};
  EXPECT_GE(ret, 0);
  if (ret >= 0) {
    EXPECT_EQ(meta.dumped_packets[MTL_SESSION_PORT_P], kMaxDumpPackets);
    dbg("%s, file_name %s\n", __func__, meta.file_name[MTL_SESSION_PORT_P]);
    struct stat st_buf = {};
    EXPECT_EQ(stat(meta.file_name[MTL_SESSION_PORT_P], &st_buf), 0);
    EXPECT_GT(st_buf.st_size, 0);
  }

  lb.stop_all();
}

TEST(St22p, put_frame_abort) {
  auto ctx = st_test_ctx();
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  constexpr uint16_t kFbCnt = 3;
  St22pLoopback lb(ctx->handle);
  St22pLoopback::Opts opts;
  opts.fb_cnt = kFbCnt;
  opts.start_tx_workers = false;
  opts.start_rx_worker = false;
  opts.start_mtl = false;
  lb.init(ctx, opts);
  if (::testing::Test::HasFatalFailure()) return;

  /* Exceed fb_cnt to verify each TX abort returns the slot to the free pool. */
  for (int i = 0; i < kFbCnt + 2; ++i) {
    struct st_frame* frame = st22p_tx_get_frame(lb.tx[0].handle.get());
    ASSERT_NE(frame, nullptr) << "no frame available on st22p TX abort cycle " << i;
    EXPECT_GE(st22p_tx_put_frame_abort(lb.tx[0].handle.get(), frame), 0);
    if (i == 0) {
      EXPECT_LT(st22p_tx_put_frame_abort(lb.tx[0].handle.get(), frame), 0);
    }
  }

  lb.start_tx_worker(0, /*block_get=*/true);
  EXPECT_GE(lb.mtl_guard.start(), 0);

  auto get_rx_frame = [&]() -> struct st_frame* {
    struct st_frame* frame = nullptr;
    mtl_test::wait_until(
        [&]() {
          frame = st22p_rx_get_frame(lb.rx[0].handle.get());
          return frame != nullptr;
        },
        /*timeout_ms=*/3000, /*poll_ms=*/1);
    return frame;
  };

  /* Exceed fb_cnt to verify each RX abort returns the slot to the RX pool. */
  for (int i = 0; i < kFbCnt + 1; ++i) {
    struct st_frame* frame = get_rx_frame();
    ASSERT_NE(frame, nullptr) << "no rx frame available on st22p abort cycle " << i;
    EXPECT_GE(st22p_rx_put_frame_abort(lb.rx[0].handle.get(), frame), 0);
    if (i == 0) {
      EXPECT_LT(st22p_rx_put_frame_abort(lb.rx[0].handle.get(), frame), 0);
    }
  }

  /* Verify normal frame delivery continues after repeated aborts. */
  struct st_frame* clean_frame = get_rx_frame();
  ASSERT_NE(clean_frame, nullptr) << "no clean rx frame available after aborts";
  EXPECT_GE(st22p_rx_put_frame(lb.rx[0].handle.get(), clean_frame), 0);

  lb.stop_all();
}

/* Written on the pipeline scheduler thread, read from the test's main thread. */
struct st22p_tx_drop_late_ctx {
  std::atomic<int> dropped_done_calls{0};
  std::atomic<int> complete_done_calls{0};
  std::atomic<int> late_calls{0};
  int done_total() const {
    return dropped_done_calls.load() + complete_done_calls.load();
  }
};

static int test_st22p_tx_drop_late_frame_done(void* priv, struct st_frame* frame) {
  auto* s = static_cast<st22p_tx_drop_late_ctx*>(priv);
  if (frame->status == ST_FRAME_STATUS_DROPPED)
    s->dropped_done_calls++;
  else if (frame->status == ST_FRAME_STATUS_COMPLETE)
    s->complete_done_calls++;
  return 0;
}

static int test_st22p_tx_drop_late_frame_late(void* priv, uint64_t /*epoch_skipped*/) {
  static_cast<st22p_tx_drop_late_ctx*>(priv)->late_calls++;
  return 0;
}

/*
 * Regression test for ST22P_TX_FLAG_DROP_WHEN_LATE end-to-end: a stale TAI
 * timestamp forces tx_st22p_if_frame_late() to drop the frame (notify_frame_done
 * DROPPED + notify_frame_late) and free its slot, while on-time frames transmit
 * normally with no late notification.
 */
TEST(St22p, tx_drop_when_late) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  constexpr int kFbCnt = 4;
  constexpr enum st_fps kFps = ST_FPS_P25;

  st22p_tx_drop_late_ctx dctx;
  auto dummy_ctx = make_st22p_context(ctx, 0, kFbCnt);

  struct st22p_tx_ops ops_tx;
  st22p_tx_ops_init(dummy_ctx.get(), &ops_tx);
  ops_tx.name = "st22p_tx_drop_when_late_test";
  ops_tx.priv = &dctx;
  ops_tx.fps = kFps;
  ops_tx.notify_frame_available = nullptr;
  ops_tx.notify_event = nullptr;
  ops_tx.flags =
      ST22P_TX_FLAG_BLOCK_GET | ST22P_TX_FLAG_USER_PACING | ST22P_TX_FLAG_DROP_WHEN_LATE;
  ops_tx.notify_frame_done = test_st22p_tx_drop_late_frame_done;
  ops_tx.notify_frame_late = test_st22p_tx_drop_late_frame_late;

  ScopedSt22pTx tx_handle(st22p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st22p_tx_set_block_timeout(tx_handle.get(), (uint64_t)NS_PER_S * 3), 0);

  mtl_test::ScopedMtlStart mtl_guard(st);
  EXPECT_GE(mtl_guard.start(), 0);

  uint64_t frame_period_ns = (uint64_t)((double)NS_PER_S / st_frame_rate(kFps));

  /* Keep a single frame in flight so tx_st22p_next_frame() drop count is deterministic.
   */
  auto put_one_and_drain = [&](uint64_t timestamp) -> bool {
    int done_before = dctx.done_total();
    struct st_frame* frame = st22p_tx_get_frame(tx_handle.get());
    if (!frame) return false;
    frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
    frame->timestamp = timestamp;
    if (st22p_tx_put_frame(tx_handle.get(), frame) < 0) return false;
    return mtl_test::wait_until([&]() { return dctx.done_total() > done_before; }, 3000,
                                1);
  };

  /* Warm-up until one on-time frame transmits, proving datapath/link is live. */
  bool link_up = false;
  auto warm_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < warm_deadline) {
    put_one_and_drain(mtl_ptp_read_time(st) + NS_PER_S / 2);
    if (dctx.complete_done_calls.load() > 0) {
      link_up = true;
      break;
    }
  }
  ASSERT_TRUE(link_up) << "datapath never came up (no frame transmitted)";

  int base_dropped = dctx.dropped_done_calls.load();
  int base_complete = dctx.complete_done_calls.load();
  int base_late = dctx.late_calls.load();

  constexpr int kOnTimeCnt = 3;
  for (int i = 0; i < kOnTimeCnt; i++) {
    EXPECT_TRUE(put_one_and_drain(mtl_ptp_read_time(st) + frame_period_ns * 4))
        << "on-time frame " << i << " never completed";
  }

  EXPECT_TRUE(put_one_and_drain(mtl_ptp_read_time(st) - NS_PER_S))
      << "stale frame never left the pipeline";

  EXPECT_EQ(dctx.dropped_done_calls.load() - base_dropped, 1);
  EXPECT_EQ(dctx.complete_done_calls.load() - base_complete, kOnTimeCnt);
  EXPECT_EQ(dctx.late_calls.load() - base_late, 1);

  EXPECT_GE(mtl_guard.stop(), 0);
}
