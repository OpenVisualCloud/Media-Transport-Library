/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "log.h"
#include "test_harness.hpp"
#include "tests.hpp"

#define ST20P_TEST_PAYLOAD_TYPE (112)
#define ST20P_TEST_UDP_PORT (20000)
#define EPOCH_DROP_TEST_UDP_PORT_ST20P (20100)

namespace {

using ScopedSt20pTx = mtl_test::ScopedHandle<st20p_tx_handle, st20p_tx_free>;
using ScopedSt20pRx = mtl_test::ScopedHandle<st20p_rx_handle, st20p_rx_free>;

/* Move-only RAII wrapper that stops and joins a worker thread on exit. */
class ScopedWorker {
 public:
  ScopedWorker() = default;
  ScopedWorker(tests_context* ctx, std::thread th,
               std::function<void()> wake_fn = nullptr)
      : ctx_(ctx), th_(std::move(th)), wake_fn_(std::move(wake_fn)) {
  }
  ~ScopedWorker() {
    stop();
  }
  ScopedWorker(const ScopedWorker&) = delete;
  ScopedWorker& operator=(const ScopedWorker&) = delete;
  ScopedWorker(ScopedWorker&& other) noexcept
      : ctx_(other.ctx_), th_(std::move(other.th_)), wake_fn_(std::move(other.wake_fn_)) {
    other.ctx_ = nullptr;
  }
  ScopedWorker& operator=(ScopedWorker&& other) noexcept {
    if (this != &other) {
      stop();
      ctx_ = other.ctx_;
      th_ = std::move(other.th_);
      wake_fn_ = std::move(other.wake_fn_);
      other.ctx_ = nullptr;
    }
    return *this;
  }

  void stop() {
    if (ctx_) {
      mtl_test::stop_worker(ctx_, th_, wake_fn_);
      ctx_ = nullptr;
    }
  }

 private:
  tests_context* ctx_ = nullptr;
  std::thread th_;
  std::function<void()> wake_fn_;
};

/* RAII guard that configures and resets global plugin fault-injection knobs. */
class ScopedPluginParams {
 public:
  ScopedPluginParams(struct st_tests_context* ctx, int fail_interval,
                     int timeout_interval, int timeout_ms)
      : ctx_(ctx) {
    st_test_jxs_fail_interval(ctx_, fail_interval);
    st_test_jxs_timeout_interval(ctx_, timeout_interval);
    st_test_jxs_timeout_ms(ctx_, timeout_ms);
  }
  ~ScopedPluginParams() {
    st_test_jxs_fail_interval(ctx_, 0);
    st_test_jxs_timeout_interval(ctx_, 0);
    st_test_jxs_timeout_ms(ctx_, 0);
  }
  ScopedPluginParams(const ScopedPluginParams&) = delete;
  ScopedPluginParams& operator=(const ScopedPluginParams&) = delete;

 private:
  struct st_tests_context* ctx_;
};

/* RAII owner for page-aligned, DMA-mapped external frame buffers. */
class ScopedExtFrames {
 public:
  ScopedExtFrames() = default;
  ~ScopedExtFrames() {
    reset();
  }
  ScopedExtFrames(const ScopedExtFrames&) = delete;
  ScopedExtFrames& operator=(const ScopedExtFrames&) = delete;
  ScopedExtFrames(ScopedExtFrames&& other) noexcept
      : st_(other.st_),
        raw_(other.raw_),
        aligned_(other.aligned_),
        iova_(other.iova_),
        map_sz_(other.map_sz_),
        frame_size_(other.frame_size_),
        frames_(std::move(other.frames_)) {
    other.st_ = nullptr;
    other.raw_ = nullptr;
    other.aligned_ = nullptr;
    other.iova_ = MTL_BAD_IOVA;
    other.map_sz_ = 0;
    other.frame_size_ = 0;
  }
  ScopedExtFrames& operator=(ScopedExtFrames&& other) noexcept {
    if (this != &other) {
      reset();
      st_ = other.st_;
      raw_ = other.raw_;
      aligned_ = other.aligned_;
      iova_ = other.iova_;
      map_sz_ = other.map_sz_;
      frame_size_ = other.frame_size_;
      frames_ = std::move(other.frames_);
      other.st_ = nullptr;
      other.raw_ = nullptr;
      other.aligned_ = nullptr;
      other.iova_ = MTL_BAD_IOVA;
      other.map_sz_ = 0;
      other.frame_size_ = 0;
    }
    return *this;
  }

  bool init(mtl_handle st, enum st_frame_fmt fmt, int width, int height, bool interlaced,
            int fb_cnt, size_t line_padding_size = 0) {
    reset();
    st_ = st;
    const uint8_t planes = st_frame_fmt_planes(fmt);
    frame_size_ = st_frame_size(fmt, width, height, interlaced) +
                  line_padding_size * static_cast<size_t>(height) * planes;
    const size_t pg_sz = mtl_page_size(st);
    const size_t total_sz = frame_size_ * static_cast<size_t>(fb_cnt);
    map_sz_ = mtl_size_page_align(total_sz, pg_sz);
    raw_ = st_test_zmalloc(map_sz_ + pg_sz);
    if (!raw_) return false;
    aligned_ =
        reinterpret_cast<uint8_t*>(MTL_ALIGN(reinterpret_cast<uint64_t>(raw_), pg_sz));
    iova_ = mtl_dma_map(st, aligned_, map_sz_);
    if (iova_ == MTL_BAD_IOVA) {
      st_test_free(raw_);
      raw_ = nullptr;
      aligned_ = nullptr;
      return false;
    }

    frames_.assign(static_cast<size_t>(fb_cnt), {});
    for (int j = 0; j < fb_cnt; j++) {
      auto& ef = frames_[static_cast<size_t>(j)];
      for (uint8_t plane = 0; plane < planes; plane++) {
        ef.linesize[plane] =
            st_frame_least_linesize(fmt, width, plane) + line_padding_size;
        if (plane == 0) {
          ef.addr[plane] = aligned_ + static_cast<size_t>(j) * frame_size_;
          ef.iova[plane] = iova_ + static_cast<size_t>(j) * frame_size_;
        } else {
          ef.addr[plane] = static_cast<uint8_t*>(ef.addr[plane - 1]) +
                           ef.linesize[plane - 1] * static_cast<size_t>(height);
          ef.iova[plane] =
              ef.iova[plane - 1] + ef.linesize[plane - 1] * static_cast<size_t>(height);
        }
      }
      ef.size = frame_size_;
      ef.opaque = nullptr;
    }
    return true;
  }

  void reset() {
    if (st_ && aligned_ && iova_ != MTL_BAD_IOVA) {
      mtl_dma_unmap(st_, aligned_, iova_, map_sz_);
    }
    if (raw_) st_test_free(raw_);
    st_ = nullptr;
    raw_ = nullptr;
    aligned_ = nullptr;
    iova_ = MTL_BAD_IOVA;
    map_sz_ = 0;
    frame_size_ = 0;
    frames_.clear();
  }

  uint8_t* data() const {
    return aligned_;
  }
  size_t frame_size() const {
    return frame_size_;
  }
  struct st_ext_frame* frames() {
    return frames_.data();
  }

 private:
  mtl_handle st_ = nullptr;
  void* raw_ = nullptr;
  uint8_t* aligned_ = nullptr;
  mtl_iova_t iova_ = MTL_BAD_IOVA;
  size_t map_sz_ = 0;
  size_t frame_size_ = 0;
  std::vector<struct st_ext_frame> frames_;
};

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
  enum st_test_level level = ST_TEST_LEVEL_MANDATORY;
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
  bool send_done_check = false;
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
  st20p_tx_handle handle = nullptr;
  std::atomic<struct st_frame*> held_frame{nullptr};
};

}  // namespace

static int test_convert_frame(struct test_converter_session* s,
                              struct st20_convert_frame_meta* frame) {
  struct st20_converter_create_req* req = &s->req;

  /* check frame sanity */
  if (frame->src->width != req->width) return -EIO;
  if (frame->dst->width != req->width) return -EIO;
  if (frame->src->height != req->height) return -EIO;
  if (frame->dst->height != req->height) return -EIO;
  if (frame->src->fmt != req->input_fmt) return -EIO;
  if (frame->dst->fmt != req->output_fmt) return -EIO;

  /* copy src sha to the start or end of convert frame */
  if (req->input_fmt == ST_FRAME_FMT_YUV422PLANAR10LE)
    memcpy(frame->dst->addr[0],
           (uint8_t*)frame->src->addr[0] + frame->src->data_size - SHA256_DIGEST_LENGTH,
           SHA256_DIGEST_LENGTH);
  else
    memcpy((uint8_t*)frame->dst->addr[0] + frame->dst->data_size - SHA256_DIGEST_LENGTH,
           frame->src->addr[0], SHA256_DIGEST_LENGTH);
  st_usleep(s->sleep_time_us);

  s->frame_cnt++;

  /* simulate fail and timeout */
  if (s->fail_interval && !(s->frame_cnt % s->fail_interval)) return -EIO;
  if (s->timeout_interval && !(s->frame_cnt % s->timeout_interval)) {
    st_usleep(s->timeout_ms * 1000);
  }

  return 0;
}

static void* test_convert_thread(void* arg) {
  struct test_converter_session* s = (struct test_converter_session*)arg;
  st20p_convert_session session_p = s->session_p;

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    struct st20_convert_frame_meta* frame = st20_converter_get_frame(session_p);
    if (!frame) {
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    int result = test_convert_frame(s, frame);
    st20_converter_put_frame(session_p, frame, result);
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
  return NULL;
}

static st20_convert_priv test_converter_create_session(
    void* priv, st20p_convert_session session_p, struct st20_converter_create_req* req) {
  struct st_tests_context* ctx = (struct st_tests_context*)priv;

  for (int i = 0; i < MAX_TEST_CONVERTER_SESSIONS; i++) {
    if (ctx->converter_sessions[i]) continue;
    auto* session = static_cast<struct test_converter_session*>(
        calloc(1, sizeof(struct test_converter_session)));
    if (!session) return NULL;
    session->idx = i;
    st_pthread_mutex_init(&session->wake_mutex, NULL);
    st_pthread_cond_init(&session->wake_cond, NULL);

    session->req = *req;
    session->session_p = session_p;
    double fps = st_frame_rate(req->fps);
    if (!fps) fps = 60;
    session->sleep_time_us = static_cast<int>(1000 * 1000 / fps / 2);
    dbg("%s(%d), sleep_time_us %d\n", __func__, i, session->sleep_time_us);
    session->fail_interval = ctx->plugin_fail_interval;
    session->timeout_interval = ctx->plugin_timeout_interval;
    session->timeout_ms = ctx->plugin_timeout_ms;

    int ret =
        pthread_create(&session->convert_thread, NULL, test_convert_thread, session);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, i, ret);
      st_pthread_mutex_destroy(&session->wake_mutex);
      st_pthread_cond_destroy(&session->wake_cond);
      free(session);
      return NULL;
    }

    ctx->converter_sessions[i] = session;
    dbg("%s(%d), input fmt: %s, output fmt: %s\n", __func__, i,
        st_frame_fmt_name(req->input_fmt), st_frame_fmt_name(req->output_fmt));
    return session;
  }

  dbg("%s, all session slot are used\n", __func__);
  return NULL;
}

static int test_converter_free_session(void* priv, st20_convert_priv session) {
  struct st_tests_context* ctx = (struct st_tests_context*)priv;
  struct test_converter_session* s = (struct test_converter_session*)session;
  int idx = s->idx;

  s->stop = true;
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);
  pthread_join(s->convert_thread, NULL);

  st_pthread_mutex_destroy(&s->wake_mutex);
  st_pthread_cond_destroy(&s->wake_cond);

  dbg("%s(%d), total %d convert frames\n", __func__, idx, s->frame_cnt);
  free(s);
  ctx->converter_sessions[idx] = NULL;
  return 0;
}

static int test_converter_frame_available(void* priv) {
  struct test_converter_session* s = (struct test_converter_session*)priv;
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);
  return 0;
}

int st_test_convert_plugin_unregister(struct st_tests_context* ctx) {
  if (ctx->converter_dev_handle) {
    st20_converter_unregister(ctx->converter_dev_handle);
    ctx->converter_dev_handle = NULL;
  }
  return 0;
}

int st_test_convert_plugin_register(struct st_tests_context* ctx) {
  auto st = ctx->handle;
  struct st20_converter_dev c_dev;
  memset(&c_dev, 0, sizeof(c_dev));
  c_dev.name = "test_converter";
  c_dev.priv = ctx;
  c_dev.target_device = ST_PLUGIN_DEVICE_TEST;
  c_dev.input_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422RFC4175PG2BE10;
  c_dev.output_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422RFC4175PG2BE10;
  c_dev.create_session = test_converter_create_session;
  c_dev.free_session = test_converter_free_session;
  c_dev.notify_frame_available = test_converter_frame_available;
  ctx->converter_dev_handle = st20_converter_register(st, &c_dev);
  if (!ctx->converter_dev_handle) {
    err("%s, converter register fail\n", __func__);
    return 0;
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
TEST(St20p, plugin_register_single) {
  plugin_register_test("libst_plugin_convert_sample.so", true);
}
TEST(St20p, plugin_register_fail) {
  plugin_register_test("/usr/local/lib/x86_64-linux-gnu/libst_plugin_sample_fail.so",
                       false);
}

static int test_st20p_frame_available(void* priv) {
  tests_context* s = (tests_context*)priv;
  s->cv.notify_all();
  return 0;
}

static int test_st20p_tx_frame_done(void* priv, struct st_frame* frame) {
  tests_context* s = (tests_context*)priv;

  if (!s->handle) return -EIO; /* not ready */

  __atomic_fetch_add(&s->fb_send_done, 1, __ATOMIC_RELAXED);

  if (!(frame->flags & ST_FRAME_FLAG_EXT_BUF)) return 0;

  for (int i = 0; i < s->fb_cnt; ++i) {
    if (frame->addr[0] == s->ext_fb + static_cast<size_t>(i) * s->frame_size) {
      s->ext_fb_in_use[i] = false;
      dbg("%s(%d), frame done at %d\n", __func__, i, s->idx);
      st20p_tx_notify_ext_frame_free((st20p_tx_handle)s->handle, frame);
      return 0;
    }
  }

  err("%s(%d), unknown frame_addr %p\n", __func__, s->idx, frame->addr[0]);
  st20p_tx_notify_ext_frame_free((st20p_tx_handle)s->handle, frame);
  return 0;
}

static int test_tx_ext_release_frame_done(void* priv, struct st_frame* frame) {
  auto* s = (tx_ext_release_ctx*)priv;
  struct st_frame* expected = nullptr;
  if (s->held_frame.compare_exchange_strong(expected, frame)) return 0;

  st20p_tx_notify_ext_frame_free(s->handle, frame);
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

TEST(St20p, tx_create_free_single) {
  pipeline_create_free_test(st20p_tx, 0, 1, 1);
}
TEST(St20p, tx_create_free_multi) {
  pipeline_create_free_test(st20p_tx, 0, 1, 6);
}
TEST(St20p, tx_create_free_mix) {
  pipeline_create_free_test(st20p_tx, 2, 3, 4);
}
TEST(St20p, rx_create_free_single) {
  pipeline_create_free_test(st20p_rx, 0, 1, 1);
}
TEST(St20p, rx_create_free_multi) {
  pipeline_create_free_test(st20p_rx, 0, 1, 6);
}
TEST(St20p, rx_create_free_mix) {
  pipeline_create_free_test(st20p_rx, 2, 3, 4);
}
TEST(St20p, tx_create_free_max) {
  pipeline_create_free_max(st20p_tx, TEST_CREATE_FREE_MAX);
}
TEST(St20p, rx_create_free_max) {
  pipeline_create_free_max(st20p_rx, TEST_CREATE_FREE_MAX);
}
TEST(St20p, tx_create_expect_fail) {
  pipeline_expect_fail_test(st20p_tx);
}
TEST(St20p, rx_create_expect_fail) {
  pipeline_expect_fail_test(st20p_rx);
}
TEST(St20p, tx_create_expect_fail_fb_cnt) {
  pipeline_expect_fail_test_fb_cnt(st20p_tx, 1);
  pipeline_expect_fail_test_fb_cnt(st20p_tx, ST20_FB_MAX_COUNT + 1);
}
TEST(St20p, rx_create_expect_fail_fb_cnt) {
  pipeline_expect_fail_test_fb_cnt(st20p_rx, 1);
  pipeline_expect_fail_test_fb_cnt(st20p_rx, ST20_FB_MAX_COUNT + 1);
}

static void test_st20p_tx_frame_thread(tests_context* s) {
  auto handle = (st20p_tx_handle)s->handle;
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
    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->width != s->width) s->incomplete_frame_cnt++;
    if (frame->height != s->height) s->incomplete_frame_cnt++;
    if (frame->fmt != s->fmt) s->incomplete_frame_cnt++;

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
    if (s->p_ext_frames) {
      int ret = st20p_tx_put_ext_frame(handle, frame, &s->p_ext_frames[s->ext_idx]);
      if (ret < 0) {
        err("%s, put ext framebuffer fail %d fb_idx %d\n", __func__, ret, s->ext_idx);
        continue;
      }
      s->ext_fb_in_use[s->ext_idx] = true;
      if (++s->ext_idx >= s->fb_cnt) s->ext_idx = 0;
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
  struct test_user_meta* meta = (struct test_user_meta*)frame->user_meta;

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
 * odd one, and a conforming FPT cannot tell the difference, so this is the only
 * wire-level check for it. The basis is the 64-bit TAI first-packet time, NOT
 * frame->rtp_timestamp: that one wraps, and 2^32 is not a multiple of the frame period in
 * ticks, so its absolute phase is meaningless. Sound but probabilistic in power, since TX
 * takes its starting parity from the wall clock -- the deterministic proof stays in
 * tests/unit/session/st20_tx/interlaced_field_epoch_test.cpp. */
static void test_st20p_rx_check_field_phase(tests_context* s, struct st_frame* frame) {
  /* a zero basis would make the check below pass for every frame, so fail instead */
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

static void test_st20p_rx_frame_thread(tests_context* s, bool internal_sha) {
  auto handle = (st20p_rx_handle)s->handle;
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

    if (frame->opaque) {
      /* free dynamic ext frame */
      bool* in_use = (bool*)frame->opaque;
      EXPECT_TRUE(*in_use);
      *in_use = false;
    }

    if (s->user_meta) test_st20p_rx_user_meta(s, frame);

    if (!st_is_frame_complete(frame->status)) {
      s->incomplete_frame_cnt++;
      st20p_rx_put_frame(handle, frame);
      continue;
    }

    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->width != s->width) s->incomplete_frame_cnt++;
    if (frame->height != s->height) s->incomplete_frame_cnt++;
    if (frame->fmt != s->fmt) s->incomplete_frame_cnt++;
    dbg("%s(%d), timestamp %" PRIu64 "\n", __func__, s->idx, frame->timestamp);
    if (frame->timestamp == timestamp) s->incomplete_frame_cnt++;
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

    st20p_rx_put_frame(handle, frame);
    __atomic_fetch_add(&s->fb_rec, 1, __ATOMIC_RELAXED);
    if (!s->start_time) s->start_time = st_test_get_monotonic_time();
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static int test_st20p_rx_query_ext_frame(void* priv, st_ext_frame* ext_frame,
                                         struct st20_rx_frame_meta* /*meta*/) {
  tests_context* s = (tests_context*)priv;
  int i = s->ext_idx;

  if (s->ext_fb_in_use[i]) {
    err("%s(%d), ext frame %d in use\n", __func__, s->idx, i);
    return -EIO;
  }

  *ext_frame = s->p_ext_frames[i];
  s->ext_fb_in_use[i] = true;
  ext_frame->opaque = &s->ext_fb_in_use[i];

  if (++s->ext_idx >= s->fb_cnt) s->ext_idx = 0;
  return 0;
}

static void populate_tx_frames(tests_context* s, st20p_tx_handle handle,
                               const St20pStreamSpec& spec,
                               const St20pDigestScenario& sc) {
  const uint8_t planes = st_frame_fmt_planes(spec.tx_fmt);
  const size_t frame_size = s->frame_size;

  for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
    uint8_t* fb = sc.tx_ext ? (s->ext_fb + static_cast<size_t>(frame) * frame_size)
                            : static_cast<uint8_t*>(st20p_tx_get_fb_addr(handle, frame));
    ASSERT_TRUE(fb != nullptr);

    if (!sc.line_padding_size) {
      st_test_rand_data(fb, frame_size, frame);
    } else {
      for (uint8_t plane = 0; plane < planes; plane++) {
        const size_t least_line = st_frame_least_linesize(spec.tx_fmt, spec.width, plane);
        uint8_t* start = static_cast<uint8_t*>(s->p_ext_frames[frame].addr[plane]);
        for (int line = 0; line < spec.height; line++) {
          uint8_t* cur =
              start + s->p_ext_frames[frame].linesize[plane] * static_cast<size_t>(line);
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
  MTL_TEST_REQUIRE_LEVEL(ctx, sc.level);
  if (sc.tx_ext || sc.rx_ext) MTL_TEST_REQUIRE_IOVA_VA(ctx);
  if (sc.pkt_convert) MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_R);

  /* Mutate plugin knobs only after all GTEST_SKIP preconditions pass. */
  ScopedPluginParams plugin_guard(ctx, sc.fail_interval, sc.timeout_interval,
                                  sc.timeout_ms);

  const size_t sessions = sc.streams.size();
  const uint8_t pt = sc.zero_payload_type ? 0 : ST20P_TEST_PAYLOAD_TYPE;

  /* Declaration order guarantees reverse RAII teardown on any ASSERT_* failure:
   * workers stop -> mtl_stop -> handles free -> contexts free -> DMA unmap -> plugin
   * reset. */
  std::vector<ScopedExtFrames> tx_ext_bufs(sessions);
  std::vector<ScopedExtFrames> rx_ext_bufs(sessions);
  std::vector<mtl_test::ScopedContext> tx_ctxs;
  std::vector<mtl_test::ScopedContext> rx_ctxs;
  std::vector<ScopedSt20pTx> tx_handles(sessions);
  std::vector<ScopedSt20pRx> rx_handles(sessions);
  mtl_test::ScopedMtlStart mtl_run(st);
  std::vector<ScopedWorker> tx_workers;
  std::vector<ScopedWorker> rx_workers;
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

    auto s = mtl_test::make_context(ctx, static_cast<int>(i), TEST_SHA_HIST_NUM);
    ASSERT_TRUE(s != nullptr);
    /* Store in tx_ctxs before st20p_tx_create so contexts outlive handles on ASSERT_*. */
    tx_ctxs.push_back(std::move(s));
    tests_context* raw_tx = tx_ctxs.back().get();
    raw_tx->width = spec.width;
    raw_tx->height = spec.height;
    raw_tx->fmt = spec.tx_fmt;
    raw_tx->user_timestamp = sc.user_timestamp;
    raw_tx->user_meta = sc.user_meta;
    raw_tx->block_get = sc.block_get;

    struct st20p_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20p_test";
    ops_tx.priv = raw_tx;
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
    ops_tx.framebuff_cnt = raw_tx->fb_cnt;
    if (sc.block_get)
      ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;
    else
      ops_tx.notify_frame_available = test_st20p_frame_available;
    ops_tx.notify_event = test_ctx_notify_event;
    ops_tx.notify_frame_done = test_st20p_tx_frame_done;
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
    raw_tx->frame_size =
        st_frame_size(spec.tx_fmt, spec.width, spec.height, sc.interlace) +
        sc.line_padding_size * static_cast<size_t>(spec.height) * planes;

    tx_handles[i].reset(st20p_tx_create(st, &ops_tx));
    ASSERT_TRUE(static_cast<bool>(tx_handles[i]));
    if (sc.block_get) {
      EXPECT_EQ(st20p_tx_set_block_timeout(tx_handles[i].get(), NS_PER_S), 0);
    }

    int sch = st20p_tx_get_sch_idx(tx_handles[i].get());
    EXPECT_GE(sch, 0);
    EXPECT_GE(mtl_sch_enable_sleep(st, sch, false), 0);

    if (sc.tx_ext) {
      ASSERT_TRUE(tx_ext_bufs[i].init(st, spec.tx_fmt, spec.width, spec.height,
                                      sc.interlace, raw_tx->fb_cnt,
                                      sc.line_padding_size));
      raw_tx->ext_fb = tx_ext_bufs[i].data();
      raw_tx->p_ext_frames = tx_ext_bufs[i].frames();
      info("%s, session %zu ext_fb %p\n", __func__, i, raw_tx->ext_fb);
    }

    populate_tx_frames(raw_tx, tx_handles[i].get(), spec, sc);
    if (::testing::Test::HasFatalFailure()) return;

    raw_tx->handle = tx_handles[i].get();
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

    auto s = mtl_test::make_context(ctx, static_cast<int>(i), TEST_SHA_HIST_NUM);
    ASSERT_TRUE(s != nullptr);
    /* Store in rx_ctxs before st20p_rx_create so contexts outlive handles on ASSERT_*. */
    rx_ctxs.push_back(std::move(s));
    tests_context* raw_rx = rx_ctxs.back().get();
    raw_rx->width = spec.width;
    raw_rx->height = spec.height;
    raw_rx->fmt = spec.rx_fmt;
    raw_rx->user_timestamp = sc.user_timestamp;
    raw_rx->user_meta = sc.user_meta;
    raw_rx->block_get = sc.block_get;
    raw_rx->rx_timing_parser = sc.rx_timing_parser;
    raw_rx->frame_size =
        st_frame_size(spec.rx_fmt, spec.width, spec.height, sc.interlace);
    memcpy(raw_rx->shas, tx_ctxs[i]->shas, TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    if (sc.rx_ext) {
      ASSERT_TRUE(rx_ext_bufs[i].init(st, spec.rx_fmt, spec.width, spec.height,
                                      sc.interlace, raw_rx->fb_cnt,
                                      sc.line_padding_size));
      raw_rx->ext_fb = rx_ext_bufs[i].data();
      raw_rx->p_ext_frames = rx_ext_bufs[i].frames();
      raw_rx->frame_size = rx_ext_bufs[i].frame_size();
      info("%s, session %zu ext_fb %p\n", __func__, i, raw_rx->ext_fb);
    }

    struct st20p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20p_test";
    ops_rx.priv = raw_rx;
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
    ops_rx.framebuff_cnt = raw_rx->fb_cnt;
    if (sc.block_get)
      ops_rx.flags |= ST20P_RX_FLAG_BLOCK_GET;
    else
      ops_rx.notify_frame_available = test_st20p_frame_available;
    if (sc.rx_timing_parser) ops_rx.flags |= ST20P_RX_FLAG_TIMING_PARSER_META;
    ops_rx.notify_event = test_ctx_notify_event;
    if (sc.rx_ext) {
      if (sc.rx_dedicated_ext) {
        ops_rx.ext_frames = raw_rx->p_ext_frames;
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

    raw_rx->handle = rx_handles[i].get();
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

  /* Steady-state measurement window replacing legacy sleep(10):
   * - warmup_frames=3 absorbs RL pacing training and pipeline startup.
   * - min_delta_frames=25 over min_window_ms=1000 gives >=25 frames at 25 fps
   *   (<=4.0% edge error) and ~60 frames at 59.94 fps (<=1.7% edge error), well
   *   inside +/-10% FPS/vsync tolerance; triggers >=8 fail/timeout intervals and
   *   hundreds of RTCP NACKs under 10% simulated packet loss.
   * - 6000 ms timeouts accommodate slower single-core kernel:lo runs. */
  const auto win = mtl_test::measure_pipeline_window(
      raw_tx_ctxs, raw_rx_ctxs, /*warmup_frames=*/3, /*min_delta_frames=*/25,
      /*min_window_ms=*/1000, /*warmup_timeout_ms=*/6000, /*window_timeout_ms=*/6000);
  EXPECT_TRUE(win.warmed_up);
  if (!win.complete) {
    info("%s, measurement window timed out before min_delta_frames\n", __func__);
  }

  if (!sc.send_done_check) {
    EXPECT_GE(mtl_run.stop(), 0);
  }

  for (size_t i = 0; i < sessions; i++) {
    const double nominal_fps = st_frame_rate(sc.streams[i].fps);
    dbg("%s(%zu,%p), vsync_cnt %d vsyncrate %f\n", __func__, i, raw_tx_ctxs[i],
        mtl_test::vsync_count(raw_tx_ctxs[i]), win.tx_vsync[i]);
    EXPECT_GT(mtl_test::vsync_count(raw_tx_ctxs[i]), 0);
    EXPECT_NEAR(win.tx_vsync[i], nominal_fps, nominal_fps * 0.1);

    tx_workers[i].stop();
    if (sc.send_done_check) {
      mtl_test::wait_until(
          [&]() {
            return mtl_test::tx_done_frames(raw_tx_ctxs[i]) >=
                   mtl_test::tx_frames(raw_tx_ctxs[i]);
          },
          1000);
      EXPECT_EQ(mtl_test::tx_frames(raw_tx_ctxs[i]),
                mtl_test::tx_done_frames(raw_tx_ctxs[i]));
    }
  }

  if (sc.send_done_check) {
    EXPECT_GE(mtl_run.stop(), 0);
  }

  const bool kernel_lo = mtl_test::is_kernel_loopback(ctx);
  for (size_t i = 0; i < sessions; i++) {
    const double nominal_fps = st_frame_rate(sc.streams[i].fps);
    dbg("%s(%zu,%p), vsync_cnt %d vsyncrate %f\n", __func__, i, raw_rx_ctxs[i],
        mtl_test::vsync_count(raw_rx_ctxs[i]), win.rx_vsync[i]);
    EXPECT_GT(mtl_test::vsync_count(raw_rx_ctxs[i]), 0);
    if (!kernel_lo) {
      EXPECT_NEAR(win.rx_vsync[i], nominal_fps, nominal_fps * 0.1);
    } else {
      info("%s, skip vsync check as it's kernel:lo\n", __func__);
    }
    rx_workers[i].stop();
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

TEST(St20p, digest_1080p_internal_s1) {
  St20pDigestScenario sc;
  sc.streams = {make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE)};
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.level = ST_TEST_LEVEL_ALL;
  sc.check_fps = false;
  st20p_rx_digest_test(sc);
}

TEST(St20p, digest_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
  };
  sc.level = ST_TEST_LEVEL_ALL;
  sc.check_fps = false;
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
  sc.level = ST_TEST_LEVEL_ALL;
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
  sc.level = ST_TEST_LEVEL_ALL;
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

TEST(St20p, tx_ext_digest_1080p_no_convert_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_YUV422RFC4175PG2BE10),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.tx_ext = true;
  sc.level = ST_TEST_LEVEL_ALL;
  st20p_rx_digest_test(sc);
}

TEST(St20p, tx_ext_digest_1080p_convert_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_V210),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.tx_ext = true;
  sc.level = ST_TEST_LEVEL_ALL;
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

TEST(St20p, rx_ext_digest_1080p_convert_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P50, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
      make_stream(ST_FPS_P59_94, 1920, 1080, ST_FRAME_FMT_V210),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.rx_ext = true;
  sc.check_fps = false;
  st20p_rx_digest_test(sc);
}

/* Packet convert writes each packet into the dst frame as it arrives, but a
 * dynamic ext frame can only be lent once its frame is known to be delivered. */
TEST(St20p, rx_create_expect_fail_packet_convert_ext_frame) {
  auto ctx = st_test_ctx();
  auto test_ctx = mtl_test::make_context(ctx, 0, 2);
  ASSERT_TRUE(test_ctx != nullptr);

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

TEST(St20p, rx_dedicated_ext_digest_1080p_convert_s2) {
  St20pDigestScenario sc;
  sc.streams = {
      make_stream(ST_FPS_P29_97, 1920, 1080, ST_FRAME_FMT_YUV422PLANAR10LE),
      make_stream(ST_FPS_P59_94, 1280, 720, ST_FRAME_FMT_Y210),
  };
  sc.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  sc.rx_ext = true;
  sc.rx_dedicated_ext = true;
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

TEST(St20p, rx_dedicated_ext_digest_1080p_convert_with_padding_s2) {
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
  sc.line_padding_size = 512;
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
  sc.level = ST_TEST_LEVEL_ALL;
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
  /* Single-plane format equal to the transport format so ctx->derive is true:
   * frames complete via tx_st20p_frame_done (not the internal-converter early
   * release in put_ext_frame), which is the path that actually parks IN_USER. */
  const enum st_frame_fmt fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;

  tx_ext_release_ctx tctx;
  ScopedExtFrames ext_bufs;
  ASSERT_TRUE(ext_bufs.init(st, fmt, width, height, false, fb_cnt));

  struct st20p_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st20p_tx_ext_release_test";
  ops_tx.priv = &tctx;
  ops_tx.port.num_port = 1;
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ST20P_TEST_UDP_PORT;
  ops_tx.port.payload_type = ST20P_TEST_PAYLOAD_TYPE;
  ops_tx.width = width;
  ops_tx.height = height;
  ops_tx.fps = ST_FPS_P59_94;
  ops_tx.input_fmt = fmt;
  ops_tx.transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops_tx.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  ops_tx.framebuff_cnt = fb_cnt;
  ops_tx.flags = ST20P_TX_FLAG_EXT_FRAME | ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE |
                 ST20P_TX_FLAG_BLOCK_GET;
  ops_tx.notify_frame_done = test_tx_ext_release_frame_done;

  ScopedSt20pTx tx_handle(st20p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  tctx.handle = tx_handle.get();
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

TEST(St20p, tx_put_frame_abort) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  auto test_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(test_ctx != nullptr);

  struct st20p_tx_ops ops_tx;
  st20p_tx_ops_init(test_ctx.get(), &ops_tx);
  ops_tx.name = "st20p_abort_test";
  ops_tx.notify_frame_available = nullptr;
  ops_tx.notify_event = nullptr;
  ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;

  ScopedSt20pTx tx_handle(st20p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st20p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);

  /* Get a frame then abort it instead of normal put. */
  struct st_frame* frame = st20p_tx_get_frame(tx_handle.get());
  if (frame) {
    EXPECT_GE(st20p_tx_put_frame_abort(tx_handle.get(), frame), 0);
    info("%s, st20p_tx_put_frame_abort succeeded\n", __func__);
  } else {
    info("%s, no frame available for TX abort test (expected in some configs)\n",
         __func__);
  }
}

TEST(St20p, rx_put_frame_abort) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(tx_ctx != nullptr);
  ASSERT_TRUE(rx_ctx != nullptr);

  struct st20p_tx_ops ops_tx;
  st20p_tx_ops_init(tx_ctx.get(), &ops_tx);
  ops_tx.name = "st20p_abort_tx";
  mtl_test::fill_tx_port(ops_tx.port, ctx, ST20P_TEST_UDP_PORT, ST20P_TEST_PAYLOAD_TYPE);
  ops_tx.notify_frame_available = nullptr;
  ops_tx.notify_event = nullptr;
  ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;

  ScopedSt20pTx tx_handle(st20p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  tx_ctx->handle = tx_handle.get();

  struct st20p_rx_ops ops_rx;
  st20p_rx_ops_init(rx_ctx.get(), &ops_rx);
  ops_rx.name = "st20p_abort_rx";
  mtl_test::fill_rx_port(ops_rx.port, ctx, ST20P_TEST_UDP_PORT, ST20P_TEST_PAYLOAD_TYPE);
  ops_rx.notify_frame_available = nullptr;
  ops_rx.notify_event = nullptr;
  ops_rx.flags |= ST20P_RX_FLAG_BLOCK_GET;

  ScopedSt20pRx rx_handle(st20p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  EXPECT_EQ(st20p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);

  mtl_test::ScopedMtlStart mtl_run(st);
  ScopedWorker tx_worker(tx_ctx.get(),
                         std::thread(
                             [](tests_context* s) {
                               auto handle = (st20p_tx_handle)s->handle;
                               while (!s->stop) {
                                 auto frame = st20p_tx_get_frame(handle);
                                 if (!frame) continue;
                                 st20p_tx_put_frame(handle, frame);
                                 __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
                               }
                             },
                             tx_ctx.get()),
                         [&]() { st20p_tx_wake_block(tx_handle.get()); });

  EXPECT_GE(mtl_run.start(), 0);
  /* Wait for TX to pace >= 10 frames (~0.17 s at 59.94 fps vs legacy sleep(2))
   * before calling st20p_rx_get_frame. */
  mtl_test::wait_until([&]() { return mtl_test::tx_frames(tx_ctx.get()) >= 10; }, 5000);

  struct st_frame* frame = st20p_rx_get_frame(rx_handle.get());
  if (frame) {
    EXPECT_GE(st20p_rx_put_frame_abort(rx_handle.get(), frame), 0);
    info("%s, st20p_rx_put_frame_abort succeeded\n", __func__);
  } else {
    info("%s, no rx frame available for abort test\n", __func__);
  }

  tx_worker.stop();
  EXPECT_GE(mtl_run.stop(), 0);
}

TEST(St20p, frame_is_late) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  struct st_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.tfmt = ST10_TIMESTAMP_FMT_TAI;
  /* Timestamp 1 second in the past is late. */
  uint64_t now = mtl_ptp_read_time(st);
  frame.timestamp = now - NS_PER_S;
  EXPECT_TRUE(st_frame_is_late(st, &frame));

  /* Timestamp 10 seconds in the future is not late. */
  frame.timestamp = now + static_cast<uint64_t>(10) * NS_PER_S;
  EXPECT_FALSE(st_frame_is_late(st, &frame));

  /* Non-TAI format always returns false. */
  frame.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame.timestamp = 0;
  EXPECT_FALSE(st_frame_is_late(st, &frame));

  info("%s, st_frame_is_late tests passed\n", __func__);
}

/*
 * Verify redundant TX and RX port stats for st20p video sessions.
 * TX frames must be counted on both ports.
 * RX bytes must be approximately equal on both ports.
 * Requires 4 ports: TX on ports 0+1, RX on ports 2+3.
 */
TEST(St20p, redundant_stats) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  MTL_TEST_REQUIRE_PORTS(ctx, 4);

  const uint16_t udp_port = ST20P_TEST_UDP_PORT + 100;
  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(tx_ctx != nullptr);
  ASSERT_TRUE(rx_ctx != nullptr);

  struct st20p_tx_ops ops_tx;
  st20p_tx_ops_init(tx_ctx.get(), &ops_tx);
  ops_tx.name = "st20p_redundant_tx";
  mtl_test::fill_tx_port(ops_tx.port, ctx, udp_port, ST20P_TEST_PAYLOAD_TYPE, 0,
                         /*redundant=*/true);
  ops_tx.notify_frame_available = nullptr;
  ops_tx.notify_event = nullptr;
  ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;
  tx_ctx->block_get = true;
  tx_ctx->width = ops_tx.width;
  tx_ctx->height = ops_tx.height;
  tx_ctx->fmt = ops_tx.input_fmt;

  ScopedSt20pTx tx_handle(st20p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st20p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);
  tx_ctx->handle = tx_handle.get();

  struct st20p_rx_ops ops_rx;
  st20p_rx_ops_init(rx_ctx.get(), &ops_rx);
  ops_rx.name = "st20p_redundant_rx";
  mtl_test::fill_rx_port(ops_rx.port, ctx, udp_port, ST20P_TEST_PAYLOAD_TYPE, 0,
                         /*redundant=*/true);
  ops_rx.notify_frame_available = nullptr;
  ops_rx.notify_event = nullptr;
  ops_rx.flags |= ST20P_RX_FLAG_BLOCK_GET;
  rx_ctx->block_get = true;
  rx_ctx->width = ops_rx.width;
  rx_ctx->height = ops_rx.height;
  rx_ctx->fmt = ops_rx.output_fmt;

  ScopedSt20pRx rx_handle(st20p_rx_create(st, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  EXPECT_EQ(st20p_rx_set_block_timeout(rx_handle.get(), NS_PER_S), 0);
  rx_ctx->handle = rx_handle.get();

  mtl_test::ScopedMtlStart mtl_run(st);
  ScopedWorker tx_worker(tx_ctx.get(),
                         std::thread(test_st20p_tx_frame_thread, tx_ctx.get()),
                         [&]() { st20p_tx_wake_block(tx_handle.get()); });
  ScopedWorker rx_worker(rx_ctx.get(),
                         std::thread(
                             [](tests_context* s) {
                               auto handle = (st20p_rx_handle)s->handle;
                               while (!s->stop) {
                                 auto frame = st20p_rx_get_frame(handle);
                                 if (!frame) continue;
                                 const bool complete =
                                     st_is_frame_complete(frame->status);
                                 st20p_rx_put_frame(handle, frame);
                                 if (complete)
                                   __atomic_fetch_add(&s->fb_rec, 1, __ATOMIC_RELAXED);
                               }
                             },
                             rx_ctx.get()),
                         [&]() { st20p_rx_wake_block(rx_handle.get()); });

  EXPECT_GE(mtl_run.start(), 0);
  /* Wait until both redundant RX legs report traffic so startup skew on leg R
   * cannot distort the 80% byte-balance check, then accumulate >= 50 frames
   * (~0.83 s at 59.94 fps vs legacy sleep(10)). */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        struct st20_rx_user_stats probe;
        if (st20p_rx_get_session_stats(rx_handle.get(), &probe) < 0) return false;
        return probe.common.port[MTL_SESSION_PORT_P].bytes > 0 &&
               probe.common.port[MTL_SESSION_PORT_R].bytes > 0;
      },
      6000));
  const int base_rx = mtl_test::rx_frames(rx_ctx.get());
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx_ctx.get()) >= base_rx + 50; }, 6000));

  tx_worker.stop();
  rx_worker.stop();
  /* Quiesce MTL tasklets before reading stats to avoid torn multi-port snapshots. */
  EXPECT_GE(mtl_run.stop(), 0);

  struct st20_tx_user_stats tx_stats;
  EXPECT_GE(st20p_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);

  struct st20_rx_user_stats rx_stats;
  EXPECT_GE(st20p_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);

  uint64_t tx_frames_p = tx_stats.common.port[MTL_SESSION_PORT_P].frames;
  uint64_t tx_frames_r = tx_stats.common.port[MTL_SESSION_PORT_R].frames;
  EXPECT_GT(tx_frames_p, static_cast<uint64_t>(0)) << "TX port 0 frames must be > 0";
  EXPECT_GT(tx_frames_r, static_cast<uint64_t>(0)) << "TX port 1 frames must be > 0";
  EXPECT_EQ(tx_frames_p, tx_frames_r)
      << "TX port frames should be equal (port0=" << tx_frames_p
      << " port1=" << tx_frames_r << ")";
  info("%s, TX frames port0=%" PRIu64 " port1=%" PRIu64 "\n", __func__, tx_frames_p,
       tx_frames_r);

  uint64_t rx_bytes_p = rx_stats.common.port[MTL_SESSION_PORT_P].bytes;
  uint64_t rx_bytes_r = rx_stats.common.port[MTL_SESSION_PORT_R].bytes;
  EXPECT_GT(rx_bytes_p, static_cast<uint64_t>(0)) << "RX port 0 bytes must be > 0";
  EXPECT_GT(rx_bytes_r, static_cast<uint64_t>(0)) << "RX port 1 bytes must be > 0";
  uint64_t rx_max_bytes = std::max(rx_bytes_p, rx_bytes_r);
  uint64_t rx_min_bytes = std::min(rx_bytes_p, rx_bytes_r);
  EXPECT_GT(rx_min_bytes, rx_max_bytes * 80 / 100)
      << "RX port bytes should be approximately equal (port0=" << rx_bytes_p
      << " port1=" << rx_bytes_r << ")";
  info("%s, RX bytes port0=%" PRIu64 " port1=%" PRIu64 "\n", __func__, rx_bytes_p,
       rx_bytes_r);

  EXPECT_GT(mtl_test::rx_frames(rx_ctx.get()), 0) << "Must have received frames";
  info("%s, TX sent %d, RX received %d frames\n", __func__,
       mtl_test::tx_frames(tx_ctx.get()), mtl_test::rx_frames(rx_ctx.get()));
}

/*
 * Concurrency reproducer: get/reset_session_stats racing with _free().
 *
 * Pre-fix: handle has no refcount/destroying flag, so a reader thread that
 * has already entered st20p_rx_get_session_stats can be left holding a
 * pointer to a session struct (and an embedded spinlock) that the main
 * thread freed via st20p_rx_free().
 *
 * Post-fix (handle refcount): readers either complete safely or get -EIO
 * from the API and stop touching the handle; the test passes.
 */
static void st20p_rx_stats_concurrent_free_test(bool reset, int reader_threads = 8,
                                                int iterations = 100) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  auto test_ctx = mtl_test::make_context(ctx, 0, 2);
  ASSERT_TRUE(test_ctx != nullptr);

  struct st20p_rx_ops ops;
  st20p_rx_ops_init(test_ctx.get(), &ops);

  std::atomic<st20p_rx_handle> live{nullptr};
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> calls{0};

  /* Reader caches the handle locally and runs a burst of 16 API calls without
   * re-checking `live`, widening the UAF window against st20p_rx_free(). */
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

TEST(St20s, rx_get_stats_concurrent_free_no_uaf) {
  st20p_rx_stats_concurrent_free_test(false);
}

TEST(St20s, rx_reset_stats_concurrent_free_no_uaf) {
  st20p_rx_stats_concurrent_free_test(true);
}

static void st20p_tx_epoch_drop_test(bool user_pacing) {
  auto ctx = st_test_ctx();
  auto st = ctx->handle;

  auto test_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(test_ctx != nullptr);

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

  /* Declare handle before ScopedMtlStart so RAII stops worker -> mtl_stop -> free,
   * while still creating the session after mtl_start (the path that reported drops). */
  ScopedSt20pTx tx_handle;
  mtl_test::ScopedMtlStart mtl_run(st);
  EXPECT_GE(mtl_run.start(), 0);

  tx_handle.reset(st20p_tx_create(st, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  EXPECT_EQ(st20p_tx_set_block_timeout(tx_handle.get(), NS_PER_S), 0);

  test_ctx->handle = tx_handle.get();
  ScopedWorker tx_worker(test_ctx.get(),
                         std::thread(
                             [user_pacing](tests_context* s) {
                               auto handle = (st20p_tx_handle)s->handle;
                               auto m_handle = s->ctx->handle;
                               while (!s->stop) {
                                 auto frame = st20p_tx_get_frame(handle);
                                 if (!frame) continue;
                                 if (user_pacing) {
                                   frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
                                   frame->timestamp = mtl_ptp_read_time(m_handle) +
                                                      20 * 1000 * 1000; /* +20ms */
                                 }
                                 st20p_tx_put_frame(handle, frame);
                                 __atomic_fetch_add(&s->fb_send, 1, __ATOMIC_RELAXED);
                               }
                             },
                             test_ctx.get()),
                         [&]() { st20p_tx_wake_block(tx_handle.get()); });

  /* Wait for >= 40 paced frames (~0.67 s at 59.94 fps vs legacy sleep(5)):
   * 40 frames exercises >13 wraparounds of the 3-slot framebuffer ring. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::tx_frames(test_ctx.get()) >= 40; }, 6000));

  tx_worker.stop();
  /* Quiesce MTL tasklets before reading session stats. */
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
