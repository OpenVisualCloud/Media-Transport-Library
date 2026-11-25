/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "log.h"
#include "test_harness.hpp"

#define ST22P_TEST_PAYLOAD_TYPE (114)
#define ST22P_TEST_UDP_PORT (16000)

namespace {

using ScopedSt22pTx = mtl_test::ScopedHandle<st22p_tx_handle, st22p_tx_free>;
using ScopedSt22pRx = mtl_test::ScopedHandle<st22p_rx_handle, st22p_rx_free>;

inline int atomic_inc(int* p) {
  return __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
}

inline void set_handle(tests_context* c, void* h) {
  __atomic_store_n(&c->handle, h, __ATOMIC_RELEASE);
}

inline void* get_handle(const tests_context* c) {
  return __atomic_load_n(&c->handle, __ATOMIC_ACQUIRE);
}

/* RAII guard declared AFTER ScopedMtlStart so worker threads are always stopped
 * and joined BEFORE mtl_stop() even on early ASSERT_* or exception unwind. */
class ScopedStop {
 public:
  explicit ScopedStop(std::function<void()> fn) : fn_(std::move(fn)) {
  }
  ~ScopedStop() {
    stop();
  }
  void stop() {
    if (done_) return;
    done_ = true;
    if (fn_) fn_();
  }
  ScopedStop(const ScopedStop&) = delete;
  ScopedStop& operator=(const ScopedStop&) = delete;

 private:
  std::function<void()> fn_;
  bool done_ = false;
};

/*
 * Steady-state measurement window for ST 2110-22 pipeline (50-59.94 fps):
 *   - warmup_frames = 3 absorbs pipeline bring-up and RL pacing training.
 *   - min_window_ms = 1000, min_delta_frames = 25 collects 50-60 frames and
 *     50-60 vsync ticks at 50-59.94 fps (<= 2.0% edge quantization error, well
 *     inside +/-10% FPS and vsync tolerances, ~9x faster than sleep(10)).
 *   - With fail_interval = 3 (2/3 * 2/3 of 59.94 fps = ~26.6 fps effective RX)
 *     or timeout_interval = 3 (~35-40 fps effective), 1.0 s processes ~55-60
 *     encoded frames (>= 18x the interval, triggering failure/timeout recovery
 *     repeatedly) while collecting ~60 vsync ticks (<= 1.7% vsync error inside
 *     +/-10%; <= 3.8% RX FPS error inside +/-50%).
 */
constexpr int kSt22pRateWarmupFrames = 3;
constexpr int kSt22pRateMinDeltaFrames = 25;
constexpr int kSt22pRateMinWindowMs = 1000;

/* RAII guard that configures test JXS plugin parameters and always restores
 * clean defaults on scope exit so one test can never pollute the next.
 * Must be constructed ONLY AFTER all GTEST_SKIP() precondition checks. */
class ScopedJxsPluginConfig {
 public:
  ScopedJxsPluginConfig(struct st_tests_context* ctx, int fail_interval,
                        int timeout_interval, int timeout_ms, int rand_ratio,
                        bool block_get)
      : ctx_(ctx) {
    st_test_jxs_fail_interval(ctx_, fail_interval);
    st_test_jxs_timeout_interval(ctx_, timeout_interval);
    st_test_jxs_timeout_ms(ctx_, timeout_ms);
    st_test_jxs_rand_ratio(ctx_, rand_ratio);
    st_test_jxs_use_block_get(ctx_, block_get);
  }
  ~ScopedJxsPluginConfig() {
    st_test_jxs_fail_interval(ctx_, 0);
    st_test_jxs_timeout_interval(ctx_, 0);
    st_test_jxs_timeout_ms(ctx_, 0);
    st_test_jxs_rand_ratio(ctx_, 0);
    st_test_jxs_use_block_get(ctx_, false);
  }
  ScopedJxsPluginConfig(const ScopedJxsPluginConfig&) = delete;
  ScopedJxsPluginConfig& operator=(const ScopedJxsPluginConfig&) = delete;

 private:
  struct st_tests_context* ctx_;
};

void alloc_ext_frames(mtl_handle st, tests_context* s_ctx, enum st_frame_fmt fmt,
                      int width, int height, size_t frame_size, bool& dma_mapped) {
  uint8_t planes = st_frame_fmt_planes(fmt);
  s_ctx->p_ext_frames =
      (struct st_ext_frame*)malloc(sizeof(*s_ctx->p_ext_frames) * s_ctx->fb_cnt);
  ASSERT_TRUE(s_ctx->p_ext_frames != NULL);

  size_t pg_sz = mtl_page_size(st);
  size_t fb_size = frame_size * s_ctx->fb_cnt;
  s_ctx->ext_fb_iova_map_sz = mtl_size_page_align(fb_size, pg_sz);
  s_ctx->ext_fb_malloc = st_test_zmalloc(s_ctx->ext_fb_iova_map_sz + pg_sz);
  ASSERT_TRUE(s_ctx->ext_fb_malloc != NULL);

  s_ctx->ext_fb = (uint8_t*)MTL_ALIGN((uint64_t)s_ctx->ext_fb_malloc, pg_sz);
  s_ctx->ext_fb_iova = mtl_dma_map(st, s_ctx->ext_fb, s_ctx->ext_fb_iova_map_sz);
  ASSERT_TRUE(s_ctx->ext_fb_iova != MTL_BAD_IOVA);
  dma_mapped = true;
  info("%s, session %d ext_fb %p\n", __func__, s_ctx->idx, s_ctx->ext_fb);

  for (int j = 0; j < s_ctx->fb_cnt; j++) {
    for (uint8_t plane = 0; plane < planes; plane++) { /* assume planes continuous */
      s_ctx->p_ext_frames[j].linesize[plane] = st_frame_least_linesize(fmt, width, plane);
      if (plane == 0) {
        s_ctx->p_ext_frames[j].addr[plane] = s_ctx->ext_fb + j * frame_size;
        s_ctx->p_ext_frames[j].iova[plane] = s_ctx->ext_fb_iova + j * frame_size;
      } else {
        s_ctx->p_ext_frames[j].addr[plane] =
            (uint8_t*)s_ctx->p_ext_frames[j].addr[plane - 1] +
            s_ctx->p_ext_frames[j].linesize[plane - 1] * height;
        s_ctx->p_ext_frames[j].iova[plane] =
            s_ctx->p_ext_frames[j].iova[plane - 1] +
            s_ctx->p_ext_frames[j].linesize[plane - 1] * height;
      }
    }
    s_ctx->p_ext_frames[j].size = frame_size;
    s_ctx->p_ext_frames[j].opaque = NULL;
  }
}

void release_ext_frames(mtl_handle st, tests_context* s_ctx, bool& dma_mapped) {
  if (!s_ctx) return;
  if (dma_mapped && s_ctx->ext_fb) {
    mtl_dma_unmap(st, s_ctx->ext_fb, s_ctx->ext_fb_iova, s_ctx->ext_fb_iova_map_sz);
    dma_mapped = false;
  }
  if (s_ctx->ext_fb_malloc) {
    st_test_free(s_ctx->ext_fb_malloc);
    s_ctx->ext_fb_malloc = nullptr;
  }
  if (s_ctx->p_ext_frames) {
    st_test_free(s_ctx->p_ext_frames);
    s_ctx->p_ext_frames = nullptr;
  }
}

struct St22pTxSession {
  mtl_handle st = nullptr;
  mtl_test::ScopedContext ctx;
  ScopedSt22pTx handle;
  std::thread worker;
  bool block_get = false;
  bool ext_dma_mapped = false;

  St22pTxSession() = default;
  St22pTxSession(St22pTxSession&& other) noexcept
      : st(other.st),
        ctx(std::move(other.ctx)),
        handle(std::move(other.handle)),
        worker(std::move(other.worker)),
        block_get(other.block_get),
        ext_dma_mapped(other.ext_dma_mapped) {
    other.st = nullptr;
    other.ext_dma_mapped = false;
  }
  St22pTxSession& operator=(St22pTxSession&&) = delete;
  St22pTxSession(const St22pTxSession&) = delete;
  St22pTxSession& operator=(const St22pTxSession&) = delete;

  void stop() {
    auto wake = [&]() {
      if (block_get && handle) st22p_tx_wake_block(handle.get());
    };
    mtl_test::stop_worker(ctx.get(), worker, wake);
  }

  ~St22pTxSession() {
    stop();
    handle.reset();
    release_ext_frames(st, ctx.get(), ext_dma_mapped);
  }
};

struct St22pRxSession {
  mtl_handle st = nullptr;
  mtl_test::ScopedContext ctx;
  ScopedSt22pRx handle;
  std::thread worker;
  bool block_get = false;
  bool ext_dma_mapped = false;

  St22pRxSession() = default;
  St22pRxSession(St22pRxSession&& other) noexcept
      : st(other.st),
        ctx(std::move(other.ctx)),
        handle(std::move(other.handle)),
        worker(std::move(other.worker)),
        block_get(other.block_get),
        ext_dma_mapped(other.ext_dma_mapped) {
    other.st = nullptr;
    other.ext_dma_mapped = false;
  }
  St22pRxSession& operator=(St22pRxSession&&) = delete;
  St22pRxSession(const St22pRxSession&) = delete;
  St22pRxSession& operator=(const St22pRxSession&) = delete;

  void stop() {
    auto wake = [&]() {
      if (block_get && handle) st22p_rx_wake_block(handle.get());
    };
    mtl_test::stop_worker(ctx.get(), worker, wake);
  }

  ~St22pRxSession() {
    stop();
    handle.reset();
    release_ext_frames(st, ctx.get(), ext_dma_mapped);
  }
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
  enum st_test_level level = ST_TEST_LEVEL_MANDATORY;
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

}  // namespace

static int test_encode_frame(struct test_st22_encoder_session* s,
                             struct st22_encode_frame_meta* frame) {
  struct st22_encoder_create_req* req = &s->req;
  size_t codestream_size = req->max_codestream_size;

  /* check frame sanity */
  if (frame->src->width != req->width) return -EIO;
  if (frame->dst->width != req->width) return -EIO;
  if (frame->src->height != req->height) return -EIO;
  if (frame->dst->height != req->height) return -EIO;
  if (frame->src->fmt != req->input_fmt) return -EIO;
  if (frame->dst->fmt != req->output_fmt) return -EIO;

  /* copy src sha to the start of encode frame */
  memcpy(frame->dst->addr[0],
         (uint8_t*)frame->src->addr[0] + frame->src->data_size - SHA256_DIGEST_LENGTH,
         SHA256_DIGEST_LENGTH);
  st_usleep(s->sleep_time_us);
  /* data size indicate the encode stream size for current frame */
  if (s->rand_ratio) {
    int rand_ratio = 100 - (rand() % s->rand_ratio);
    codestream_size = codestream_size * rand_ratio / 100;
  }
  frame->dst->data_size = codestream_size;

  s->frame_cnt++;
  dbg("%s(%d), succ, codestream_size %" PRIu64 "\n", __func__, s->idx, codestream_size);

  /* simulate fail and timeout */
  if (s->fail_interval) {
    if (!(s->frame_cnt % s->fail_interval)) {
      return -EIO;
    }
  }
  if (s->timeout_interval) {
    if (!(s->frame_cnt % s->timeout_interval)) {
      st_usleep(s->timeout_ms * 1000);
    }
  }

  return 0;
}

static void* test_encode_thread(void* arg) {
  struct test_st22_encoder_session* s = (struct test_st22_encoder_session*)arg;
  st22p_encode_session session_p = s->session_p;
  struct st_tests_context* ctx = s->ctx;
  struct st22_encode_frame_meta* frame;
  int result;

  if (ctx->encoder_use_block_get) st22_encoder_set_block_timeout(session_p, NS_PER_S);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22_encoder_get_frame(session_p);
    if (!frame) { /* no frame */
      if (!ctx->encoder_use_block_get) {
        st_pthread_mutex_lock(&s->wake_mutex);
        if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
        st_pthread_mutex_unlock(&s->wake_mutex);
      }
      continue;
    }
    result = test_encode_frame(s, frame);
    st22_encoder_put_frame(session_p, frame, result);
  }
  dbg("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static st22_encode_priv test_encoder_create_session(void* priv,
                                                    st22p_encode_session session_p,
                                                    struct st22_encoder_create_req* req) {
  struct st_tests_context* ctx = (struct st_tests_context*)priv;
  struct test_st22_encoder_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_TEST_ENCODER_SESSIONS; i++) {
    if (ctx->encoder_sessions[i]) continue;
    session = (struct test_st22_encoder_session*)malloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->ctx = ctx;
    session->idx = i;
    st_pthread_mutex_init(&session->wake_mutex, NULL);
    st_pthread_cond_init(&session->wake_cond, NULL);

    req->max_codestream_size = req->codestream_size;
    if (ctx->encoder_use_block_get) req->resp_flag |= ST22_ENCODER_RESP_FLAG_BLOCK_GET;

    session->req = *req;
    session->session_p = session_p;
    double fps = st_frame_rate(req->fps);
    if (!fps) fps = 60;
    session->sleep_time_us = 1000 * 1000 / fps * 8 / 10;
    dbg("%s(%d), sleep_time_us %d\n", __func__, i, session->sleep_time_us);
    session->fail_interval = ctx->plugin_fail_interval;
    session->timeout_interval = ctx->plugin_timeout_interval;
    session->timeout_ms = ctx->plugin_timeout_ms;
    session->rand_ratio = ctx->plugin_rand_ratio;

    ret = pthread_create(&session->encode_thread, NULL, test_encode_thread, session);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, i, ret);
      st_pthread_mutex_destroy(&session->wake_mutex);
      st_pthread_cond_destroy(&session->wake_cond);
      free(session);
      return NULL;
    }

    ctx->encoder_sessions[i] = session;
    dbg("%s(%d), input fmt: %s, output fmt: %s\n", __func__, i,
        st_frame_fmt_name(req->input_fmt), st_frame_fmt_name(req->output_fmt));
    return session;
  }

  dbg("%s, all session slot are used\n", __func__);
  return NULL;
}

static int test_encoder_free_session(void* priv, st22_encode_priv session) {
  struct st_tests_context* ctx = (struct st_tests_context*)priv;
  struct test_st22_encoder_session* encoder_session =
      (struct test_st22_encoder_session*)session;
  int idx = encoder_session->idx;

  encoder_session->stop = true;
  if (ctx->encoder_use_block_get) {
    st22_encoder_wake_block(encoder_session->session_p);
  } else {
    st_pthread_mutex_lock(&encoder_session->wake_mutex);
    st_pthread_cond_signal(&encoder_session->wake_cond);
    st_pthread_mutex_unlock(&encoder_session->wake_mutex);
  }
  pthread_join(encoder_session->encode_thread, NULL);

  st_pthread_mutex_destroy(&encoder_session->wake_mutex);
  st_pthread_cond_destroy(&encoder_session->wake_cond);

  dbg("%s(%d), total %d encode frames\n", __func__, idx, encoder_session->frame_cnt);
  free(encoder_session);
  ctx->encoder_sessions[idx] = NULL;
  return 0;
}

static int test_encoder_frame_available(void* priv) {
  struct test_st22_encoder_session* s = (struct test_st22_encoder_session*)priv;
  struct st_tests_context* ctx = s->ctx;

  if (ctx->encoder_use_block_get) return 0;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int test_decode_frame(struct test_st22_decoder_session* s,
                             struct st22_decode_frame_meta* frame) {
  struct st22_decoder_create_req* req = &s->req;

  /* check frame sanity */
  if (frame->src->width != req->width) return -EIO;
  if (frame->dst->width != req->width) return -EIO;
  if (frame->src->height != req->height) return -EIO;
  if (frame->dst->height != req->height) return -EIO;
  if (frame->src->fmt != req->input_fmt) return -EIO;
  if (frame->dst->fmt != req->output_fmt) return -EIO;
  if (frame->src->data_size > frame->src->buffer_size) return -EIO;

  /* copy sha to the end of decode frame */
  memcpy((uint8_t*)frame->dst->addr[0] + frame->dst->data_size - SHA256_DIGEST_LENGTH,
         frame->src->addr[0], SHA256_DIGEST_LENGTH);
  st_usleep(s->sleep_time_us);

  s->frame_cnt++;

  /* simulate fail and timeout */
  if (s->fail_interval) {
    if (!(s->frame_cnt % s->fail_interval)) {
      return -EIO;
    }
  }
  if (s->timeout_interval) {
    if (!(s->frame_cnt % s->timeout_interval)) {
      st_usleep(s->timeout_ms * 1000);
    }
  }

  return 0;
}

static void* test_decode_thread(void* arg) {
  struct test_st22_decoder_session* s = (struct test_st22_decoder_session*)arg;
  st22p_decode_session session_p = s->session_p;
  struct st_tests_context* ctx = s->ctx;
  struct st22_decode_frame_meta* frame;
  int result;

  if (ctx->decoder_use_block_get) st22_decoder_set_block_timeout(session_p, NS_PER_S);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22_decoder_get_frame(session_p);
    if (!frame) { /* no frame */
      if (!ctx->decoder_use_block_get) {
        st_pthread_mutex_lock(&s->wake_mutex);
        if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
        st_pthread_mutex_unlock(&s->wake_mutex);
      }
      continue;
    }
    result = test_decode_frame(s, frame);
    st22_decoder_put_frame(session_p, frame, result);
  }
  dbg("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static st22_decode_priv test_decoder_create_session(void* priv,
                                                    st22p_decode_session session_p,
                                                    struct st22_decoder_create_req* req) {
  struct st_tests_context* ctx = (struct st_tests_context*)priv;
  struct test_st22_decoder_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_TEST_DECODER_SESSIONS; i++) {
    if (ctx->decoder_sessions[i]) continue;
    session = (struct test_st22_decoder_session*)malloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->idx = i;
    session->ctx = ctx;
    st_pthread_mutex_init(&session->wake_mutex, NULL);
    st_pthread_cond_init(&session->wake_cond, NULL);

    if (ctx->decoder_use_block_get) {
      req->resp_flag |= ST22_DECODER_RESP_FLAG_BLOCK_GET;
    }

    session->req = *req;
    session->session_p = session_p;
    double fps = st_frame_rate(req->fps);
    if (!fps) fps = 60;
    session->sleep_time_us = 1000 * 1000 / fps * 8 / 10;
    dbg("%s(%d), sleep_time_us %d\n", __func__, i, session->sleep_time_us);
    session->fail_interval = ctx->plugin_fail_interval;
    session->timeout_interval = ctx->plugin_timeout_interval;
    session->timeout_ms = ctx->plugin_timeout_ms;

    ret = pthread_create(&session->decode_thread, NULL, test_decode_thread, session);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, i, ret);
      st_pthread_mutex_destroy(&session->wake_mutex);
      st_pthread_cond_destroy(&session->wake_cond);
      free(session);
      return NULL;
    }

    ctx->decoder_sessions[i] = session;
    dbg("%s(%d), input fmt: %s, output fmt: %s\n", __func__, i,
        st_frame_fmt_name(req->input_fmt), st_frame_fmt_name(req->output_fmt));
    return session;
  }

  dbg("%s, all session slot are used\n", __func__);
  return NULL;
}

static int test_decoder_free_session(void* priv, st22_decode_priv session) {
  struct st_tests_context* ctx = (struct st_tests_context*)priv;
  struct test_st22_decoder_session* decoder_session =
      (struct test_st22_decoder_session*)session;
  int idx = decoder_session->idx;

  decoder_session->stop = true;
  if (ctx->decoder_use_block_get) {
    st22_decoder_wake_block(decoder_session->session_p);
  } else {
    st_pthread_mutex_lock(&decoder_session->wake_mutex);
    st_pthread_cond_signal(&decoder_session->wake_cond);
    st_pthread_mutex_unlock(&decoder_session->wake_mutex);
  }
  pthread_join(decoder_session->decode_thread, NULL);

  st_pthread_mutex_destroy(&decoder_session->wake_mutex);
  st_pthread_cond_destroy(&decoder_session->wake_cond);

  dbg("%s(%d), total %d decode frames\n", __func__, idx, decoder_session->frame_cnt);
  free(decoder_session);
  ctx->decoder_sessions[idx] = NULL;
  return 0;
}

static int test_decoder_frame_available(void* priv) {
  struct test_st22_decoder_session* s = (struct test_st22_decoder_session*)priv;
  struct st_tests_context* ctx = s->ctx;

  if (ctx->decoder_use_block_get) return 0;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

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
  int ret = 0;

  struct st22_decoder_dev d_dev;
  memset(&d_dev, 0, sizeof(d_dev));
  d_dev.name = "st22_test_decoder";
  d_dev.priv = ctx;
  d_dev.target_device = ST_PLUGIN_DEVICE_TEST;
  d_dev.input_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM | ST_FMT_CAP_H264_CBR_CODESTREAM;
  d_dev.output_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422PLANAR8;
  d_dev.create_session = test_decoder_create_session;
  d_dev.free_session = test_decoder_free_session;
  d_dev.notify_frame_available = test_decoder_frame_available;
  ctx->decoder_dev_handle = st22_decoder_register(st, &d_dev);
  if (!ctx->decoder_dev_handle) {
    err("%s, decoder register fail\n", __func__);
    return ret;
  }

  struct st22_encoder_dev e_dev;
  memset(&e_dev, 0, sizeof(e_dev));
  e_dev.name = "st22_test_encoder";
  e_dev.priv = ctx;
  e_dev.target_device = ST_PLUGIN_DEVICE_TEST;
  e_dev.input_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422PLANAR8;
  e_dev.output_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM | ST_FMT_CAP_H264_CBR_CODESTREAM;
  e_dev.create_session = test_encoder_create_session;
  e_dev.free_session = test_encoder_free_session;
  e_dev.notify_frame_available = test_encoder_frame_available;
  ctx->encoder_dev_handle = st22_encoder_register(st, &e_dev);
  if (!ctx->encoder_dev_handle) {
    err("%s, encoder register fail\n", __func__);
    return ret;
  }

  info("%s, succ\n", __func__);
  return 0;
}

static void plugin_register_test(const char* so_name, bool expect_succ) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

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
TEST(St22p, plugin_register_single) {
  plugin_register_test("libst_plugin_convert_sample.so", true);
}
TEST(St22p, plugin_register_fail) {
  plugin_register_test("/usr/local/lib/x86_64-linux-gnu/libst_plugin_sample_fail.so",
                       false);
}

static void frame_draw_logo_test(enum st_frame_fmt fmt, uint32_t w, uint32_t h,
                                 uint32_t logo_w, uint32_t logo_h, uint32_t x, uint32_t y,
                                 bool expect) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  size_t logo_size = st_frame_size(fmt, logo_w, logo_h, false);
  size_t frame_size = st_frame_size(fmt, w, h, false);
  auto hp_free = [st](void* p) {
    if (p) mtl_hp_free(st, p);
  };
  std::unique_ptr<void, decltype(hp_free)> frame_buf(
      mtl_hp_malloc(st, frame_size, MTL_PORT_P), hp_free);
  ASSERT_TRUE(frame_buf != nullptr) << "frame_buf malloc fail";
  std::unique_ptr<void, decltype(hp_free)> logo_buf(
      mtl_hp_malloc(st, logo_size, MTL_PORT_P), hp_free);
  ASSERT_TRUE(logo_buf != nullptr) << "logo_buf malloc fail";

  struct st_frame frame_meta;
  struct st_frame logo_meta;
  frame_meta.addr[0] = frame_buf.get();
  frame_meta.fmt = fmt;
  frame_meta.width = w;
  frame_meta.height = h;
  logo_meta.addr[0] = logo_buf.get();
  logo_meta.fmt = fmt;
  logo_meta.width = logo_w;
  logo_meta.height = logo_h;

  int ret = st_draw_logo(&frame_meta, &logo_meta, x, y);
  if (expect)
    EXPECT_GE(ret, 0);
  else
    EXPECT_LT(ret, 0);
}

TEST(St22p, draw_logo_rfc4175_1080p) {
  frame_draw_logo_test(ST_FRAME_FMT_YUV422RFC4175PG2BE10, 1920, 1080, 200, 200, 16, 16,
                       true);
}

TEST(St22p, draw_logo_rfc4175_1080p_full) {
  frame_draw_logo_test(ST_FRAME_FMT_YUV422RFC4175PG2BE10, 1920, 1080, 1920, 1080, 0, 0,
                       true);
}

TEST(St22p, draw_logo_rfc4175_1080p_fail) {
  frame_draw_logo_test(ST_FRAME_FMT_YUV422RFC4175PG2BE10, 1920, 1080, 1920, 1080, 100,
                       100, false);
}

static int test_st22p_tx_frame_available(void* priv) {
  tests_context* s = (tests_context*)priv;
  std::lock_guard<std::mutex> lck(s->mtx);
  s->cv.notify_all();
  return 0;
}

static int test_st22p_tx_frame_done(void* priv, struct st_frame* frame) {
  tests_context* s = (tests_context*)priv;

  if (!get_handle(s)) return -EIO; /* not ready */

  atomic_inc(&s->fb_send_done);

  if (!(frame->flags & ST_FRAME_FLAG_EXT_BUF)) return 0;

  for (int i = 0; i < s->fb_cnt; ++i) {
    if (frame->addr[0] == s->ext_fb + i * s->frame_size) {
      __atomic_store_n(&s->ext_fb_in_use[i], false, __ATOMIC_RELEASE);
      dbg("%s(%d), frame done at %d\n", __func__, i, s->idx);
      return 0;
    }
  }

  err("%s(%d), unknown frame_addr %p\n", __func__, s->idx, frame->addr[0]);
  return 0;
}

static int test_st22p_rx_frame_available(void* priv) {
  tests_context* s = (tests_context*)priv;
  std::lock_guard<std::mutex> lck(s->mtx);
  s->cv.notify_all();
  return 0;
}

static int test_st22p_rx_query_ext_frame(void* priv, st_ext_frame* ext_frame,
                                         struct st22_rx_frame_meta* /*meta*/) {
  auto ctx = (tests_context*)priv;
  if (!get_handle(ctx)) return -EIO; /* not ready */
  int i = ctx->ext_idx;

  /* check ext_fb_in_use */
  if (__atomic_load_n(&ctx->ext_fb_in_use[i], __ATOMIC_ACQUIRE)) {
    err("%s(%d), ext frame %d in use\n", __func__, ctx->idx, i);
    return -EIO;
  }
  *ext_frame = ctx->p_ext_frames[i];

  dbg("%s(%d), set ext frame %d(%p) to use\n", __func__, ctx->idx, i, ext_frame->addr);
  __atomic_store_n(&ctx->ext_fb_in_use[i], true, __ATOMIC_RELEASE);

  ext_frame->opaque = &ctx->ext_fb_in_use[i];

  if (++ctx->ext_idx >= ctx->fb_cnt) ctx->ext_idx = 0;
  return 0;
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
  ops_tx->notify_frame_available = test_st22p_tx_frame_available;
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
  ops_rx->notify_frame_available = test_st22p_rx_frame_available;
  st22->frame_size =
      st_frame_size(ops_rx->output_fmt, ops_rx->width, ops_rx->height, false);
  ops_rx->notify_event = test_ctx_notify_event;
}

static void st22p_tx_assert_cnt(int expect_s22_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st22_tx_sessions_cnt, expect_s22_tx_cnt);
}

static void st22p_rx_assert_cnt(int expect_s22_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st22_rx_sessions_cnt, expect_s22_rx_cnt);
}

TEST(St22p, tx_create_free_single) {
  pipeline_create_free_test(st22p_tx, 0, 1, 1);
}
TEST(St22p, tx_create_free_multi) {
  pipeline_create_free_test(st22p_tx, 0, 1, 6);
}
TEST(St22p, tx_create_free_mix) {
  pipeline_create_free_test(st22p_tx, 2, 3, 4);
}
TEST(St22p, rx_create_free_single) {
  pipeline_create_free_test(st22p_rx, 0, 1, 1);
}
TEST(St22p, rx_create_free_multi) {
  pipeline_create_free_test(st22p_rx, 0, 1, 6);
}
TEST(St22p, rx_create_free_mix) {
  pipeline_create_free_test(st22p_rx, 2, 3, 4);
}
TEST(St22p, tx_create_free_max) {
  pipeline_create_free_max(st22p_tx, TEST_CREATE_FREE_MAX);
}
TEST(St22p, rx_create_free_max) {
  pipeline_create_free_max(st22p_rx, TEST_CREATE_FREE_MAX);
}
TEST(St22p, tx_create_expect_fail) {
  pipeline_expect_fail_test(st22p_tx);
}
TEST(St22p, rx_create_expect_fail) {
  pipeline_expect_fail_test(st22p_rx);
}
TEST(St22p, tx_create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  pipeline_expect_fail_test_fb_cnt(st22p_tx, fbcnt);
  fbcnt = ST22_FB_MAX_COUNT + 1;
  pipeline_expect_fail_test_fb_cnt(st22p_tx, fbcnt);
}
TEST(St22p, rx_create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  pipeline_expect_fail_test_fb_cnt(st22p_rx, fbcnt);
  fbcnt = ST22_FB_MAX_COUNT + 1;
  pipeline_expect_fail_test_fb_cnt(st22p_rx, fbcnt);
}

static void test_st22p_tx_frame_thread(void* args) {
  tests_context* s = (tests_context*)args;
  auto handle = (st22p_tx_handle)get_handle(s);
  struct st_frame* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22p_tx_get_frame(handle);
    if (!frame) { /* no frame */
      if (!s->block_get) {
        lck.lock();
        frame = st22p_tx_get_frame(handle);
        if (!frame) {
          if (!s->stop) s->cv.wait(lck);
          lck.unlock();
          continue;
        }
        lck.unlock();
      } else {
        continue;
      }
    }
    if (frame->data_size != s->frame_size) atomic_inc(&s->incomplete_frame_cnt);
    if (frame->buffer_size != s->frame_size) atomic_inc(&s->incomplete_frame_cnt);
    if (frame->width != s->width) atomic_inc(&s->incomplete_frame_cnt);
    if (frame->height != s->height) atomic_inc(&s->incomplete_frame_cnt);
    if (frame->fmt != s->fmt) atomic_inc(&s->incomplete_frame_cnt);
    if (s->user_timestamp) {
      const int sent = mtl_test::tx_frames(s);
      frame->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
      frame->timestamp = sent + 1; /* add one to avoid zero timestamp */
      dbg("%s(%d), timestamp %d\n", __func__, s->idx, sent);
    }
    if (s->p_ext_frames) {
      const int idx = s->ext_idx;
      __atomic_store_n(&s->ext_fb_in_use[idx], true, __ATOMIC_RELEASE);
      int ret = st22p_tx_put_ext_frame(handle, frame, &s->p_ext_frames[idx]);
      if (ret < 0) {
        __atomic_store_n(&s->ext_fb_in_use[idx], false, __ATOMIC_RELEASE);
        err("%s, put ext framebuffer fail %d fb_idx %d\n", __func__, ret, idx);
        continue;
      }
      s->ext_idx = (idx + 1 >= s->fb_cnt) ? 0 : (idx + 1);
    } else {
      st22p_tx_put_frame(handle, frame);
    }
    atomic_inc(&s->fb_send);
    if (!s->start_time) {
      s->start_time = st_test_get_monotonic_time();
      dbg("%s(%d), start_time %" PRIu64 "\n", __func__, s->idx, s->start_time);
    }
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void test_st22p_rx_frame_thread(void* args) {
  tests_context* s = (tests_context*)args;
  auto handle = (st22p_rx_handle)get_handle(s);
  struct st_frame* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22p_rx_get_frame(handle);
    if (!frame) { /* no frame */
      if (!s->block_get) {
        lck.lock();
        frame = st22p_rx_get_frame(handle);
        if (!frame) {
          if (!s->stop) s->cv.wait(lck);
          lck.unlock();
          continue;
        }
        lck.unlock();
      } else {
        continue;
      }
    }

    if (frame->data_size != s->frame_size) atomic_inc(&s->incomplete_frame_cnt);
    if (frame->buffer_size < s->frame_size) atomic_inc(&s->incomplete_frame_cnt);
    if (frame->width != s->width) atomic_inc(&s->incomplete_frame_cnt);
    if (frame->height != s->height) atomic_inc(&s->incomplete_frame_cnt);
    if (frame->fmt != s->fmt) atomic_inc(&s->incomplete_frame_cnt);
    dbg("%s(%d), timestamp %" PRIu64 "\n", __func__, s->idx, frame->timestamp);
    if (frame->timestamp == 0) atomic_inc(&s->incomplete_frame_cnt);

    if (frame->opaque) {
      /* free dynamic ext frame */
      bool* in_use = (bool*)frame->opaque;
      EXPECT_TRUE(__atomic_load_n(in_use, __ATOMIC_ACQUIRE));
      __atomic_store_n(in_use, false, __ATOMIC_RELEASE);
    }

    /* check user timestamp if it has */
    if (s->user_timestamp && !s->user_pacing) {
      if (s->pre_timestamp) {
        /*
         * some frame may drop as SHA256 is slow,
         * just check timestamp is adding with small step
         */
        if (((uint32_t)frame->timestamp - s->pre_timestamp) > 4) {
          atomic_inc(&s->incomplete_frame_cnt);
          err("%s(%d), frame user timestamp %" PRIu64 " pre_timestamp %u\n", __func__,
              s->idx, frame->timestamp, s->pre_timestamp);
        }
      }
      s->pre_timestamp = (uint32_t)frame->timestamp;
    }

    unsigned char* sha =
        (unsigned char*)frame->addr[0] + frame->data_size - SHA256_DIGEST_LENGTH;
    int i = 0;
    for (i = 0; i < ST22_TEST_SHA_HIST_NUM; i++) {
      unsigned char* target_sha = s->shas[i];
      if (!memcmp(sha, target_sha, SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= ST22_TEST_SHA_HIST_NUM) {
      test_sha_dump("st22p_rx_error_sha", sha);
      atomic_inc(&s->sha_fail_cnt);
    }
    st22p_rx_put_frame(handle, frame);
    atomic_inc(&s->fb_rec);
    if (!s->start_time) s->start_time = st_test_get_monotonic_time();
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void st22p_rx_digest_test(const St22pDigestScenario& sc) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  /* All GTEST_SKIP() precondition checks MUST precede plugin state mutation. */
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_LEVEL(ctx, sc.level);
  if (sc.tx_ext || sc.rx_ext) {
    MTL_TEST_REQUIRE_IOVA_VA(ctx);
  }

  ScopedJxsPluginConfig plugin_cfg(ctx, sc.fail_interval, sc.timeout_interval,
                                   sc.timeout_ms, sc.rand_ratio, sc.codec_block_get);

  const size_t sessions = sc.streams.size();
  std::vector<St22pTxSession> tx;
  std::vector<St22pRxSession> rx;
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
    s.st = st;
    s.block_get = sc.block_get;
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), ST22_TEST_SHA_HIST_NUM);
    s.ctx->width = stream.width;
    s.ctx->height = stream.height;
    s.ctx->fmt = stream.fmt;
    s.ctx->user_timestamp = sc.user_timestamp;
    s.ctx->block_get = sc.block_get;

    struct st22p_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22p_test";
    ops_tx.priv = s.ctx.get();
    mtl_test::fill_tx_port(ops_tx.port, ctx, udp_port, ST22P_TEST_PAYLOAD_TYPE, sc.ssrc);
    ops_tx.width = stream.width;
    ops_tx.height = stream.height;
    ops_tx.fps = stream.fps;
    ops_tx.interlaced = sc.interlace;
    ops_tx.input_fmt = stream.fmt;
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.codec = stream.codec;
    ops_tx.device = ST_PLUGIN_DEVICE_TEST;
    ops_tx.quality = ST22_QUALITY_MODE_QUALITY;
    ops_tx.framebuff_cnt = s.ctx->fb_cnt;
    if (sc.block_get)
      ops_tx.flags |= ST22P_TX_FLAG_BLOCK_GET;
    else
      ops_tx.notify_frame_available = test_st22p_tx_frame_available;
    ops_tx.notify_event = test_ctx_notify_event;
    ops_tx.notify_frame_done = test_st22p_tx_frame_done;
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
      ASSERT_NO_FATAL_FAILURE(alloc_ext_frames(st, s.ctx.get(), stream.fmt, stream.width,
                                               stream.height, s.ctx->frame_size,
                                               s.ext_dma_mapped));
    }

    /* Populate TX framebuffers and embed SHA256 digest at tail of each frame. */
    size_t frame_size = s.ctx->frame_size;
    for (int frame = 0; frame < ST22_TEST_SHA_HIST_NUM; frame++) {
      uint8_t* fb = sc.tx_ext ? (s.ctx->ext_fb + frame * frame_size)
                              : (uint8_t*)st22p_tx_get_fb_addr(s.handle.get(), frame);
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = s.ctx->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st22p_tx", result);
      memcpy(fb + frame_size - SHA256_DIGEST_LENGTH, result, SHA256_DIGEST_LENGTH);
    }

    set_handle(s.ctx.get(), s.handle.get());
    s.worker = std::thread(test_st22p_tx_frame_thread, s.ctx.get());
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
    s.st = st;
    s.block_get = sc.block_get;
    s.ctx = mtl_test::make_context(ctx, static_cast<int>(i), ST22_TEST_SHA_HIST_NUM);
    s.ctx->width = stream.width;
    s.ctx->height = stream.height;
    s.ctx->fmt = stream.fmt;
    s.ctx->user_timestamp = sc.user_timestamp;
    s.ctx->block_get = sc.block_get;
    memcpy(s.ctx->shas, tx[i].ctx->shas, ST22_TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    if (sc.rx_ext) {
      size_t ext_frame_size =
          st_frame_size(stream.fmt, stream.width, stream.height, sc.interlace);
      ASSERT_NO_FATAL_FAILURE(alloc_ext_frames(st, s.ctx.get(), stream.fmt, stream.width,
                                               stream.height, ext_frame_size,
                                               s.ext_dma_mapped));
    }

    struct st22p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22p_test";
    ops_rx.priv = s.ctx.get();
    mtl_test::fill_rx_port(ops_rx.port, ctx, udp_port, ST22P_TEST_PAYLOAD_TYPE, sc.ssrc);
    ops_rx.width = stream.width;
    ops_rx.height = stream.height;
    ops_rx.fps = stream.fps;
    ops_rx.interlaced = sc.interlace;
    ops_rx.output_fmt = stream.fmt;
    ops_rx.pack_type = ST22_PACK_CODESTREAM;
    ops_rx.codec = stream.codec;
    ops_rx.device = ST_PLUGIN_DEVICE_TEST;
    ops_rx.framebuff_cnt = s.ctx->fb_cnt;
    if (sc.block_get)
      ops_rx.flags |= ST22P_RX_FLAG_BLOCK_GET;
    else
      ops_rx.notify_frame_available = test_st22p_rx_frame_available;
    ops_rx.notify_event = test_ctx_notify_event;
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

    if (sc.derive)
      s.ctx->frame_size = tx[i].ctx->frame_size;
    else
      s.ctx->frame_size = st_frame_size(ops_rx.output_fmt, ops_rx.width, ops_rx.height,
                                        ops_rx.interlaced);

    s.handle.reset(st22p_rx_create(st, &ops_rx));
    ASSERT_TRUE(static_cast<bool>(s.handle));
    if (!sc.derive) {
      EXPECT_EQ(s.ctx->frame_size, st22p_rx_frame_size(s.handle.get()));
    }
    if (sc.block_get) {
      EXPECT_EQ(st22p_rx_set_block_timeout(s.handle.get(), NS_PER_S), 0);
    }

    set_handle(s.ctx.get(), s.handle.get());
    s.worker = std::thread(test_st22p_rx_frame_thread, s.ctx.get());

    struct st_queue_meta meta;
    EXPECT_GE(st22p_rx_get_queue_meta(s.handle.get(), &meta), 0);
  }

  mtl_test::ScopedMtlStart mtl_guard(st);
  ScopedStop workers_stop([&]() {
    for (auto& s : tx) s.stop();
    for (auto& s : rx) s.stop();
  });
  EXPECT_GE(mtl_guard.start(), 0);

  /* Capture steady-state FPS and vsync rates BEFORE teardown (--auto_start_stop safe). */
  auto tx_ctxs = collect_ctxs(tx);
  auto rx_ctxs = collect_ctxs(rx);
  auto win =
      mtl_test::measure_pipeline_window(tx_ctxs, rx_ctxs, kSt22pRateWarmupFrames,
                                        kSt22pRateMinDeltaFrames, kSt22pRateMinWindowMs);
  EXPECT_TRUE(win.warmed_up);
  EXPECT_TRUE(win.complete);

  workers_stop.stop();
  EXPECT_GE(mtl_guard.stop(), 0);

  for (size_t i = 0; i < sessions; i++) {
    const double nominal_fps = st_frame_rate(sc.streams[i].fps);
    const int tx_vsync = mtl_test::vsync_count(tx[i].ctx.get());
    const int rx_vsync = mtl_test::vsync_count(rx[i].ctx.get());
    const int tx_fb_send = mtl_test::tx_frames(tx[i].ctx.get());
    const int rx_fb_rec = mtl_test::rx_frames(rx[i].ctx.get());

    if (sc.vsync) {
      dbg("%s(%zu), tx vsync_cnt %d vsyncrate %f\n", __func__, i, tx_vsync,
          win.tx_vsync[i]);
      EXPECT_GT(tx_vsync, 0);
      EXPECT_NEAR(win.tx_vsync[i], nominal_fps, nominal_fps * 0.1);

      dbg("%s(%zu), rx vsync_cnt %d vsyncrate %f\n", __func__, i, rx_vsync,
          win.rx_vsync[i]);
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

TEST(St22p, digest_st22_1080p_s1) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 10},
  };
  sc.level = ST_TEST_LEVEL_ALL;
  st22p_rx_digest_test(sc);
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

TEST(St22p, digest_st22_4k_s1) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920 * 2, 1080 * 2, ST_FRAME_FMT_YUV422PLANAR10LE,
       ST22_CODEC_JPEGXS, 20},
  };
  sc.level = ST_TEST_LEVEL_ALL;
  st22p_rx_digest_test(sc);
}

TEST(St22p, digest_st22_s2) {
  St22pDigestScenario sc;
  sc.streams = {
      {ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 10},
      {ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE, ST22_CODEC_JPEGXS, 16},
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

TEST(St22p, tx_put_frame_abort) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  St22pTxSession tx;
  tx.st = st;
  tx.block_get = true;
  tx.ctx = mtl_test::make_context(ctx, 0, 3);

  struct st22p_tx_ops ops_tx;
  st22p_tx_ops_init(tx.ctx.get(), &ops_tx);
  ops_tx.name = "st22p_abort_test";
  ops_tx.notify_frame_available = nullptr;
  ops_tx.notify_event = nullptr;
  ops_tx.flags |= ST22P_TX_FLAG_BLOCK_GET;

  tx.handle.reset(st22p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx.handle));
  EXPECT_EQ(st22p_tx_set_block_timeout(tx.handle.get(), NS_PER_S), 0);

  struct st_frame* frame = st22p_tx_get_frame(tx.handle.get());
  if (frame) {
    EXPECT_GE(st22p_tx_put_frame_abort(tx.handle.get(), frame), 0);
    info("%s, st22p_tx_put_frame_abort succeeded\n", __func__);
  } else {
    info("%s, no frame available for st22p TX abort test\n", __func__);
  }
}

TEST(St22p, rx_put_frame_abort) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  St22pTxSession tx;
  tx.st = st;
  tx.block_get = true;
  tx.ctx = mtl_test::make_context(ctx, 0, 3);

  struct st22p_tx_ops ops_tx;
  st22p_tx_ops_init(tx.ctx.get(), &ops_tx);
  ops_tx.name = "st22p_abort_tx";
  mtl_test::fill_tx_port(ops_tx.port, ctx, ST22P_TEST_UDP_PORT, ST22P_TEST_PAYLOAD_TYPE);
  ops_tx.notify_frame_available = nullptr;
  ops_tx.notify_event = nullptr;
  ops_tx.flags |= ST22P_TX_FLAG_BLOCK_GET;

  tx.handle.reset(st22p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx.handle));
  EXPECT_EQ(st22p_tx_set_block_timeout(tx.handle.get(), NS_PER_S), 0);
  set_handle(tx.ctx.get(), tx.handle.get());
  tx.worker = std::thread(
      [](tests_context* s) {
        auto handle = (st22p_tx_handle)get_handle(s);
        while (!s->stop) {
          auto frame = st22p_tx_get_frame(handle);
          if (!frame) continue;
          st22p_tx_put_frame(handle, frame);
          atomic_inc(&s->fb_send);
        }
      },
      tx.ctx.get());

  St22pRxSession rx;
  rx.st = st;
  rx.block_get = true;
  rx.ctx = mtl_test::make_context(ctx, 0, 3);

  struct st22p_rx_ops ops_rx;
  st22p_rx_ops_init(rx.ctx.get(), &ops_rx);
  ops_rx.name = "st22p_abort_rx";
  mtl_test::fill_rx_port(ops_rx.port, ctx, ST22P_TEST_UDP_PORT, ST22P_TEST_PAYLOAD_TYPE);
  ops_rx.notify_frame_available = nullptr;
  ops_rx.notify_event = nullptr;
  ops_rx.flags |= ST22P_RX_FLAG_BLOCK_GET;

  rx.handle.reset(st22p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx.handle));
  EXPECT_EQ(st22p_rx_set_block_timeout(rx.handle.get(), NS_PER_S), 0);

  mtl_test::ScopedMtlStart mtl_guard(st);
  ScopedStop workers_stop([&]() { tx.stop(); });
  EXPECT_GE(mtl_guard.start(), 0);

  /* Block-get returns as soon as the first decoded frame arrives (~50 ms). */
  struct st_frame* frame = nullptr;
  mtl_test::wait_until(
      [&]() {
        frame = st22p_rx_get_frame(rx.handle.get());
        return frame != nullptr;
      },
      /*timeout_ms=*/3000, /*poll_ms=*/1);

  if (frame) {
    EXPECT_GE(st22p_rx_put_frame_abort(rx.handle.get(), frame), 0);
    info("%s, st22p_rx_put_frame_abort succeeded\n", __func__);
  } else {
    info("%s, no rx frame available for st22p abort test\n", __func__);
  }

  workers_stop.stop();
  EXPECT_GE(mtl_guard.stop(), 0);
}

/* written on the pipeline scheduler thread, read from the test's main thread,
 * so they must be atomic. */
struct st22p_tx_drop_late_ctx {
  std::atomic<int> dropped_done_calls{0};
  std::atomic<int> complete_done_calls{0};
  std::atomic<int> late_calls{0};
  /* total frames that have left the pipeline (transmitted or dropped) */
  int done_total() const {
    return dropped_done_calls.load() + complete_done_calls.load();
  }
};

static int test_st22p_tx_drop_late_frame_done(void* priv, struct st_frame* frame) {
  auto* s = (st22p_tx_drop_late_ctx*)priv;
  if (frame->status == ST_FRAME_STATUS_DROPPED)
    s->dropped_done_calls++;
  else if (frame->status == ST_FRAME_STATUS_COMPLETE)
    s->complete_done_calls++;
  return 0;
}

static int test_st22p_tx_drop_late_frame_late(void* priv, uint64_t epoch_skipped) {
  auto* s = (st22p_tx_drop_late_ctx*)priv;
  (void)epoch_skipped;
  s->late_calls++;
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

  const int fb_cnt = 4;
  const int width = 1920, height = 1080;
  const enum st_frame_fmt fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  const enum st_fps fps = ST_FPS_P25;

  st22p_tx_drop_late_ctx dctx;

  struct st22p_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st22p_tx_drop_when_late_test";
  ops_tx.priv = &dctx;
  ops_tx.port.num_port = 1;
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ST22P_TEST_UDP_PORT;
  ops_tx.port.payload_type = ST22P_TEST_PAYLOAD_TYPE;
  ops_tx.width = width;
  ops_tx.height = height;
  ops_tx.fps = fps;
  ops_tx.input_fmt = fmt;
  ops_tx.pack_type = ST22_PACK_CODESTREAM;
  ops_tx.codec = ST22_CODEC_JPEGXS;
  ops_tx.device = ST_PLUGIN_DEVICE_TEST;
  ops_tx.quality = ST22_QUALITY_MODE_QUALITY;
  ops_tx.framebuff_cnt = fb_cnt;
  ops_tx.codestream_size = st_frame_size(fmt, width, height, false) / 8;
  ops_tx.flags =
      ST22P_TX_FLAG_BLOCK_GET | ST22P_TX_FLAG_USER_PACING | ST22P_TX_FLAG_DROP_WHEN_LATE;
  ops_tx.notify_frame_done = test_st22p_tx_drop_late_frame_done;
  ops_tx.notify_frame_late = test_st22p_tx_drop_late_frame_late;

  ScopedSt22pTx tx_handle(st22p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st22p_tx_set_block_timeout(tx_handle.get(), (uint64_t)NS_PER_S * 3), 0);

  mtl_test::ScopedMtlStart mtl_guard(st);
  EXPECT_GE(mtl_guard.start(), 0);

  uint64_t frame_period_ns = (uint64_t)((double)NS_PER_S / st_frame_rate(fps));

  /* Produce exactly one frame and block until it leaves the pipeline (either
   * transmitted or dropped). Keeping a single frame in flight is essential:
   * tx_st22p_next_frame() always services the NEWEST ENCODED frame, so with
   * several queued at once the older on-time frames would miss their own TX
   * window and be dropped too, making the drop count non-deterministic. */
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

  /* Warm-up: the first frames race session negotiation / link-up. Pump on-time
   * frames one at a time until one actually transmits, proving the datapath is
   * live, then snapshot the counters so link-up jitter cannot taint the asserts.
   * The half-second margin keeps a warm-up frame from being ruled late while the
   * link is still coming up. */
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

  /* Measured phase, datapath already live. On-time frames sit a few periods in
   * the future so pipeline latency cannot make them late; each must transmit. */
  const int on_time_cnt = 3;
  for (int i = 0; i < on_time_cnt; i++) {
    EXPECT_TRUE(put_one_and_drain(mtl_ptp_read_time(st) + frame_period_ns * 4))
        << "on-time frame " << i << " never completed";
  }

  /* One clearly-stale frame: tx_st22p_if_frame_late() must drop it
   * (notify_frame_done DROPPED + notify_frame_late) and free its slot. */
  EXPECT_TRUE(put_one_and_drain(mtl_ptp_read_time(st) - NS_PER_S))
      << "stale frame never left the pipeline";

  EXPECT_EQ(dctx.dropped_done_calls.load() - base_dropped, 1);
  EXPECT_EQ(dctx.complete_done_calls.load() - base_complete, on_time_cnt);
  EXPECT_EQ(dctx.late_calls.load() - base_late, 1);

  EXPECT_GE(mtl_guard.stop(), 0);
}
