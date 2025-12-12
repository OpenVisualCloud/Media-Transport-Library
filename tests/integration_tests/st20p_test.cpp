/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <thread>

#include "log.h"
#include "tests.hpp"

#define ST20P_TEST_PAYLOAD_TYPE (112)
#define ST20P_TEST_UDP_PORT (20000)

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

  /* copy src sha to the start of convert frame */
  if (req->input_fmt == ST_FRAME_FMT_YUV422PLANAR10LE)
    memcpy(frame->dst->addr[0],
           (uint8_t*)frame->src->addr[0] + frame->src->data_size - SHA256_DIGEST_LENGTH,
           SHA256_DIGEST_LENGTH);
  else
    memcpy((uint8_t*)frame->dst->addr[0] + frame->dst->data_size - SHA256_DIGEST_LENGTH,
           frame->src->addr[0], SHA256_DIGEST_LENGTH);
  st_usleep(s->sleep_time_us);

  s->frame_cnt++;
  // dbg("%s(%d), succ\n", __func__, s->idx);

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

static void* test_convert_thread(void* arg) {
  struct test_converter_session* s = (struct test_converter_session*)arg;
  st20p_convert_session session_p = s->session_p;
  struct st20_convert_frame_meta* frame;
  int result;

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20_converter_get_frame(session_p);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    result = test_convert_frame(s, frame);
    st20_converter_put_frame(session_p, frame, result);
  }
  dbg("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static st20_convert_priv test_converter_create_session(
    void* priv, st20p_convert_session session_p, struct st20_converter_create_req* req) {
  struct st_tests_context* ctx = (struct st_tests_context*)priv;
  struct test_converter_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_TEST_CONVERTER_SESSIONS; i++) {
    if (ctx->converter_sessions[i]) continue;
    session = (struct test_converter_session*)malloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->idx = i;
    st_pthread_mutex_init(&session->wake_mutex, NULL);
    st_pthread_cond_init(&session->wake_cond, NULL);

    session->req = *req;
    session->session_p = session_p;
    double fps = st_frame_rate(req->fps);
    if (!fps) fps = 60;
    session->sleep_time_us = 1000 * 1000 / fps / 2;
    dbg("%s(%d), sleep_time_us %d\n", __func__, i, session->sleep_time_us);
    session->fail_interval = ctx->plugin_fail_interval;
    session->timeout_interval = ctx->plugin_timeout_interval;
    session->timeout_ms = ctx->plugin_timeout_ms;

    ret = pthread_create(&session->convert_thread, NULL, test_convert_thread, session);
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
  struct test_converter_session* converter_sessions =
      (struct test_converter_session*)session;
  int idx = converter_sessions->idx;

  converter_sessions->stop = true;
  st_pthread_mutex_lock(&converter_sessions->wake_mutex);
  st_pthread_cond_signal(&converter_sessions->wake_cond);
  st_pthread_mutex_unlock(&converter_sessions->wake_mutex);
  pthread_join(converter_sessions->convert_thread, NULL);

  st_pthread_mutex_destroy(&converter_sessions->wake_mutex);
  st_pthread_cond_destroy(&converter_sessions->wake_cond);

  dbg("%s(%d), total %d convert frames\n", __func__, idx, converter_sessions->frame_cnt);
  free(converter_sessions);
  ctx->converter_sessions[idx] = NULL;
  return 0;
}

static int test_converter_frame_available(void* priv) {
  struct test_converter_session* s = (struct test_converter_session*)priv;

  // dbg("%s(%d)\n", __func__, s->idx);
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
  int ret = 0;

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

  if (ret < 0) {
    EXPECT_EQ(pre_nb, new_nb);
  } else {
    EXPECT_EQ(pre_nb + 1, new_nb);
  }
}

TEST(St20p, plugin_register_single) {
  plugin_register_test("/usr/local/lib/x86_64-linux-gnu/libst_plugin_sample.so", true);
}
TEST(St20p, plugin_register_fail) {
  plugin_register_test("/usr/local/lib/x86_64-linux-gnu/libst_plugin_sample_fail.so",
                       false);
}

static int test_st20p_tx_frame_available(void* priv) {
  tests_context* s = (tests_context*)priv;

  s->cv.notify_all();

  return 0;
}

static int test_st20p_tx_frame_done(void* priv, struct st_frame* frame) {
  tests_context* s = (tests_context*)priv;

  if (!s->handle) return -EIO; /* not ready */

  s->fb_send_done++;

  if (!(frame->flags & ST_FRAME_FLAG_EXT_BUF)) return 0;

  for (int i = 0; i < s->fb_cnt; ++i) {
    if (frame->addr[0] == s->ext_fb + i * s->frame_size) {
      s->ext_fb_in_use[i] = false;
      dbg("%s(%d), frame done at %d\n", __func__, i, s->idx);
      return 0;
    }
  }

  err("%s(%d), unknown frame_addr %p\n", __func__, s->idx, frame->addr[0]);
  return 0;
}

static int test_st20p_rx_frame_available(void* priv) {
  tests_context* s = (tests_context*)priv;

  s->cv.notify_all();

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
  ops_tx->notify_frame_available = test_st20p_tx_frame_available;
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
  ops_rx->notify_frame_available = test_st20p_rx_frame_available;
  ops_rx->notify_event = test_ctx_notify_event;
  st20->frame_size = st_frame_size(ops_rx->output_fmt, ops_rx->width, ops_rx->height,
                                   ops_rx->interlaced);
}

static void st20p_tx_assert_cnt(int expect_st20_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(var.st20_tx_sessions_cnt, expect_st20_tx_cnt);
}

static void st20p_rx_assert_cnt(int expect_st20_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_var_info var;
  int ret;

  ret = st_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
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
  uint16_t fbcnt = 1;
  pipeline_expect_fail_test_fb_cnt(st20p_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  pipeline_expect_fail_test_fb_cnt(st20p_tx, fbcnt);
}
TEST(St20p, rx_create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  pipeline_expect_fail_test_fb_cnt(st20p_rx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  pipeline_expect_fail_test_fb_cnt(st20p_rx, fbcnt);
}

static void test_st20p_tx_frame_thread(void* args) {
  tests_context* s = (tests_context*)args;
  auto handle = s->handle;
  struct st_frame* frame;
  struct test_user_meta meta;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_tx_get_frame((st20p_tx_handle)handle);
    if (!frame) { /* no frame */
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait(lck);
        lck.unlock();
      }
      continue;
    }
    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->width != s->width) s->incomplete_frame_cnt++;
    if (frame->height != s->height) s->incomplete_frame_cnt++;
    if (frame->fmt != s->fmt) s->incomplete_frame_cnt++;
    if (s->user_timestamp) {
      frame->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
      frame->timestamp = s->fb_send;
      dbg("%s(%d), timestamp %d\n", __func__, s->idx, s->fb_send);
    }
    if (s->user_meta) {
      meta.magic = TEST_USER_META_MAGIC;
      meta.session_idx = s->idx;
      meta.frame_idx = s->fb_send;
      frame->user_meta = &meta;
      frame->user_meta_size = sizeof(meta);
    }
    if (s->p_ext_frames) {
      int ret = st20p_tx_put_ext_frame((st20p_tx_handle)handle, frame,
                                       &s->p_ext_frames[s->ext_idx]);
      if (ret < 0) {
        err("%s, put ext framebuffer fail %d fb_idx %d\n", __func__, ret, s->ext_idx);
        continue;
      }
      s->ext_fb_in_use[s->ext_idx] = true;
      s->ext_idx++;
      if (s->ext_idx >= s->fb_cnt) s->ext_idx = 0;
    } else {
      /* directly put */
      st20p_tx_put_frame((st20p_tx_handle)handle, frame);
    }
    s->fb_send++;
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

static void test_st20p_rx_frame_thread(void* args) {
  tests_context* s = (tests_context*)args;
  auto handle = s->handle;
  struct st_frame* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);
  uint64_t timestamp = 0;

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_rx_get_frame((st20p_rx_handle)handle);
    if (!frame) { /* no frame */
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait(lck);
        lck.unlock();
      }
      continue;
    }

    if (s->user_meta) test_st20p_rx_user_meta(s, frame);

    if (!st_is_frame_complete(frame->status)) {
      s->incomplete_frame_cnt++;
      st20p_rx_put_frame((st20p_rx_handle)handle, frame);
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
    if (s->rx_timing_parser) {
      if (!frame->tp[MTL_SESSION_PORT_P]) s->incomplete_frame_cnt++;
    }

    /* check user timestamp if it has */
    if (s->user_timestamp && !s->user_pacing) {
      if (s->pre_timestamp) {
        /*
         * some frame may drop as SHA256 is slow,
         * just check timestamp is adding with small step
         */
        if (((uint32_t)frame->timestamp - s->pre_timestamp) > 4) {
          s->incomplete_frame_cnt++;
          err("%s(%d), frame user timestamp %" PRIu64 " pre_timestamp %u\n", __func__,
              s->idx, frame->timestamp, s->pre_timestamp);
        }
      }
      s->pre_timestamp = (uint32_t)frame->timestamp;
    }

    unsigned char* sha =
        (unsigned char*)frame->addr[0] + frame->data_size - SHA256_DIGEST_LENGTH;
    int i = 0;
    for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
      unsigned char* target_sha = s->shas[i];
      if (!memcmp(sha, target_sha, SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= TEST_SHA_HIST_NUM) {
      test_sha_dump("st20p_rx_error_sha", sha);
      s->sha_fail_cnt++;
    }
    /* directly put */
    st20p_rx_put_frame((st20p_rx_handle)handle, frame);
    s->fb_rec++;
    if (!s->start_time) s->start_time = st_test_get_monotonic_time();
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void test_internal_st20p_rx_frame_thread(void* args) {
  tests_context* s = (tests_context*)args;
  auto handle = s->handle;
  struct st_frame* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);
  uint64_t timestamp = 0;
  unsigned char result[SHA256_DIGEST_LENGTH];

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_rx_get_frame((st20p_rx_handle)handle);
    if (!frame) { /* no frame */
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait(lck);
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
      st20p_rx_put_frame((st20p_rx_handle)handle, frame);
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

    /* check user timestamp if it has */
    if (s->user_timestamp && !s->user_pacing) {
      if (s->pre_timestamp) {
        /*
         * some frame may drop as SHA256 is slow,
         * just check timestamp is adding with small step
         */
        if (((uint32_t)frame->timestamp - s->pre_timestamp) > 4) {
          s->incomplete_frame_cnt++;
          err("%s(%d), frame user timestamp %" PRIu64 " pre_timestamp %u\n", __func__,
              s->idx, frame->timestamp, s->pre_timestamp);
        }
      }
      s->pre_timestamp = (uint32_t)frame->timestamp;
    }

    int i = 0;
    unsigned char* fb = (unsigned char*)frame->addr[0];
    SHA256(fb, s->frame_size, result);
    for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
      unsigned char* target_sha = s->shas[i];
      if (!memcmp(result, target_sha, SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= TEST_SHA_HIST_NUM) {
      test_sha_dump("st20p_rx_error_sha", result);
      s->sha_fail_cnt++;
    }
    /* directly put */
    st20p_rx_put_frame((st20p_rx_handle)handle, frame);
    s->fb_rec++;
    if (!s->start_time) s->start_time = st_test_get_monotonic_time();
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static int test_st20p_rx_query_ext_frame(void* priv, st_ext_frame* ext_frame,
                                         struct st20_rx_frame_meta* meta) {
  tests_context* s = (tests_context*)priv;
  int i = s->ext_idx;

  /* check ext_fb_in_use */
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

struct st20p_rx_digest_test_para {
  enum st_plugin_device device;
  int sessions;
  int fail_interval;
  int timeout_interval;
  int timeout_ms;
  bool tx_ext;
  bool rx_ext;
  bool rx_dedicated_ext;
  bool check_fps;
  enum st_test_level level;
  int fb_cnt;
  bool user_timestamp;
  bool vsync;
  bool pkt_convert;
  size_t line_padding_size;
  bool send_done_check;
  bool interlace;
  bool user_meta;
  bool rtcp;
  enum st20_packing packing;
  enum st21_pacing pacing;
  uint32_t ssrc;
  bool block_get;
  bool rx_timing_parser;
  bool rx_auto_detect;
  bool zero_payload_type;
};

static void test_st20p_init_rx_digest_para(struct st20p_rx_digest_test_para* para) {
  memset(para, 0, sizeof(*para));

  para->device = ST_PLUGIN_DEVICE_TEST;
  para->sessions = 1;
  para->fail_interval = 0;
  para->timeout_interval = 0;
  para->timeout_ms = 0;
  para->tx_ext = false;
  para->rx_ext = false;
  para->rx_dedicated_ext = false;
  para->check_fps = true;
  para->level = ST_TEST_LEVEL_MANDATORY;
  para->user_timestamp = false;
  para->vsync = true;
  para->pkt_convert = false;
  para->line_padding_size = 0;
  para->send_done_check = false;
  para->interlace = false;
  para->user_meta = false;
  para->rtcp = false;
  para->packing = ST20_PACKING_BPM;
  para->pacing = ST21_PACING_NARROW;
  para->ssrc = 0;
  para->block_get = false;
  para->rx_auto_detect = false;
  para->zero_payload_type = false;
}

static void st20p_rx_digest_test(enum st_fps fps[], int width[], int height[],
                                 enum st_frame_fmt tx_fmt[], enum st20_fmt t_fmt[],
                                 enum st_frame_fmt rx_fmt[],
                                 struct st20p_rx_digest_test_para* para) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  int ret;
  struct st20p_tx_ops ops_tx;
  struct st20p_rx_ops ops_rx;
  int sessions = para->sessions;

  st_test_jxs_fail_interval(ctx, para->fail_interval);
  st_test_jxs_timeout_interval(ctx, para->timeout_interval);
  st_test_jxs_timeout_ms(ctx, para->timeout_ms);

  if (ctx->para.num_ports < 2) {
    info("%s, dual port should be enabled, one for tx and one for rx\n", __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  /* return if level lower than global */
  if (para->level < ctx->level) return;

  if (para->tx_ext || para->rx_ext) {
    if (ctx->iova == MTL_IOVA_MODE_PA) {
      info("%s, skip ext_buf test as it's PA iova mode\n", __func__);
      return;
    }
  }

  if (para->pkt_convert) {
    enum mtl_pmd_type pmd = ctx->para.pmd[MTL_PORT_R];
    if (MTL_PMD_DPDK_USER != pmd) {
      info("%s, skip as pmd %d is not dpdk user\n", __func__, pmd);
      return;
    }
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20p_tx_handle> tx_handle;
  std::vector<st20p_rx_handle> rx_handle;
  std::vector<double> expect_framerate_tx;
  std::vector<double> expect_framerate_rx;
  std::vector<double> framerate_tx;
  std::vector<double> framerate_rx;
  std::vector<double> vsyncrate_tx;
  std::vector<double> vsyncrate_rx;
  std::vector<std::thread> tx_thread;
  std::vector<std::thread> rx_thread;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate_tx.resize(sessions);
  expect_framerate_rx.resize(sessions);
  framerate_tx.resize(sessions);
  framerate_rx.resize(sessions);
  vsyncrate_tx.resize(sessions);
  vsyncrate_rx.resize(sessions);
  tx_thread.resize(sessions);
  rx_thread.resize(sessions);

  struct st20p_thread_guard {
    std::vector<tests_context*>& tx_ctx;
    std::vector<tests_context*>& rx_ctx;
    std::vector<std::thread>& tx_thread;
    std::vector<std::thread>& rx_thread;
    std::vector<st20p_tx_handle>& tx_handle;
    std::vector<st20p_rx_handle>& rx_handle;
    st20p_rx_digest_test_para* para;

    ~st20p_thread_guard() {
      for (size_t i = 0; i < tx_ctx.size(); i++) {
        auto* ctx = tx_ctx[i];
        if (!ctx) continue;
        ctx->stop = true;
        if (para && para->block_get && i < tx_handle.size() && tx_handle[i])
          st20p_tx_wake_block(tx_handle[i]);
        ctx->cv.notify_all();
      }

      for (size_t i = 0; i < rx_ctx.size(); i++) {
        auto* ctx = rx_ctx[i];
        if (!ctx) continue;
        ctx->stop = true;
        if (para && para->block_get && i < rx_handle.size() && rx_handle[i])
          st20p_rx_wake_block(rx_handle[i]);
        ctx->cv.notify_all();
      }

      for (auto& t : tx_thread) {
        if (t.joinable()) t.join();
      }
      for (auto& t : rx_thread) {
        if (t.joinable()) t.join();
      }
    }
  } guard{test_ctx_tx, test_ctx_rx, tx_thread, rx_thread, tx_handle, rx_handle, para};

  for (int i = 0; i < sessions; i++) {
    expect_framerate_tx[i] = st_frame_rate(fps[i]);
    if (para->timeout_interval) {
      expect_framerate_tx[i] =
          expect_framerate_tx[i] * (para->timeout_interval - 1) / para->timeout_interval;
    }

    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->width = width[i];
    test_ctx_tx[i]->height = height[i];
    test_ctx_tx[i]->fmt = tx_fmt[i];
    test_ctx_tx[i]->user_timestamp = para->user_timestamp;
    test_ctx_tx[i]->user_meta = para->user_meta;
    test_ctx_tx[i]->block_get = para->block_get;

    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20p_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.port.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_R],
             MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ST20P_TEST_UDP_PORT + i * 2;
    ops_tx.port.payload_type = para->zero_payload_type ? 0 : ST20P_TEST_PAYLOAD_TYPE;
    ops_tx.port.ssrc = para->ssrc;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.input_fmt = tx_fmt[i];
    ops_tx.interlaced = para->interlace;
    ops_tx.transport_packing = para->packing;
    ops_tx.transport_pacing = para->pacing;
    ops_tx.transport_fmt = t_fmt[i];
    ops_tx.transport_linesize = 0;
    ops_tx.device = para->device;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    if (para->block_get)
      ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;
    else
      ops_tx.notify_frame_available = test_st20p_tx_frame_available;
    ops_tx.notify_event = test_ctx_notify_event;
    ops_tx.notify_frame_done = test_st20p_tx_frame_done;
    if (para->tx_ext) {
      ops_tx.flags |= ST20P_TX_FLAG_EXT_FRAME;
    }
    if (para->user_timestamp) ops_tx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
    if (para->vsync) ops_tx.flags |= ST20P_TX_FLAG_ENABLE_VSYNC;

    if (para->rtcp) {
      ops_tx.flags |= ST20P_TX_FLAG_ENABLE_RTCP;
      ops_tx.rtcp.buffer_size = 1024;
    }

    uint8_t planes = st_frame_fmt_planes(tx_fmt[i]);
    test_ctx_tx[i]->frame_size =
        st_frame_size(tx_fmt[i], width[i], height[i], ops_tx.interlaced) +
        para->line_padding_size * height[i] * planes;

    tx_handle[i] = st20p_tx_create(st, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    if (para->block_get) {
      ret = st20p_tx_set_block_timeout(tx_handle[i], NS_PER_S);
      EXPECT_EQ(ret, 0);
    }

    int sch = st20p_tx_get_sch_idx(tx_handle[i]);
    EXPECT_GE(sch, 0);
    ret = mtl_sch_enable_sleep(st, sch, false);
    EXPECT_GE(ret, 0);

    /* sha calculate */
    size_t frame_size = test_ctx_tx[i]->frame_size;
    uint8_t* fb;

    /* init ext frames, only for no convert */
    if (para->tx_ext) {
      test_ctx_tx[i]->p_ext_frames = (struct st_ext_frame*)malloc(
          sizeof(*test_ctx_tx[i]->p_ext_frames) * test_ctx_tx[i]->fb_cnt);
      size_t pg_sz = mtl_page_size(st);
      size_t fb_size = frame_size * test_ctx_tx[i]->fb_cnt;
      test_ctx_tx[i]->ext_fb_iova_map_sz =
          mtl_size_page_align(fb_size, pg_sz); /* align */
      size_t fb_size_malloc = test_ctx_tx[i]->ext_fb_iova_map_sz + pg_sz;
      test_ctx_tx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
      ASSERT_TRUE(test_ctx_tx[i]->ext_fb_malloc != NULL);
      test_ctx_tx[i]->ext_fb =
          (uint8_t*)MTL_ALIGN((uint64_t)test_ctx_tx[i]->ext_fb_malloc, pg_sz);
      test_ctx_tx[i]->ext_fb_iova =
          mtl_dma_map(st, test_ctx_tx[i]->ext_fb, test_ctx_tx[i]->ext_fb_iova_map_sz);
      ASSERT_TRUE(test_ctx_tx[i]->ext_fb_iova != MTL_BAD_IOVA);
      info("%s, session %d ext_fb %p\n", __func__, i, test_ctx_tx[i]->ext_fb);

      for (int j = 0; j < test_ctx_tx[i]->fb_cnt; j++) {
        for (uint8_t plane = 0; plane < planes; plane++) { /* assume planes continuous */
          test_ctx_tx[i]->p_ext_frames[j].linesize[plane] =
              st_frame_least_linesize(rx_fmt[i], width[i], plane) +
              para->line_padding_size;
          if (plane == 0) {
            test_ctx_tx[i]->p_ext_frames[j].addr[plane] =
                test_ctx_tx[i]->ext_fb + j * frame_size;
            test_ctx_tx[i]->p_ext_frames[j].iova[plane] =
                test_ctx_tx[i]->ext_fb_iova + j * frame_size;
          } else {
            test_ctx_tx[i]->p_ext_frames[j].addr[plane] =
                (uint8_t*)test_ctx_tx[i]->p_ext_frames[j].addr[plane - 1] +
                test_ctx_tx[i]->p_ext_frames[j].linesize[plane - 1] * height[i];
            test_ctx_tx[i]->p_ext_frames[j].iova[plane] =
                test_ctx_tx[i]->p_ext_frames[j].iova[plane - 1] +
                test_ctx_tx[i]->p_ext_frames[j].linesize[plane - 1] * height[i];
          }
        }
        test_ctx_tx[i]->p_ext_frames[j].size = frame_size;
        test_ctx_tx[i]->p_ext_frames[j].opaque = NULL;
      }
    }

    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      if (para->tx_ext)
        fb = (uint8_t*)test_ctx_tx[i]->ext_fb + frame * frame_size;
      else
        fb = (uint8_t*)st20p_tx_get_fb_addr(tx_handle[i], frame);
      ASSERT_TRUE(fb != NULL);
      if (!para->line_padding_size)
        st_test_rand_data(fb, frame_size, frame);
      else {
        for (int plane = 0; plane < planes; plane++) {
          size_t least_line_size = st_frame_least_linesize(tx_fmt[i], width[i], plane);
          uint8_t* start = (uint8_t*)test_ctx_tx[i]->p_ext_frames[frame].addr[plane];
          for (int line = 0; line < height[i]; line++) {
            uint8_t* cur_line =
                start + test_ctx_tx[i]->p_ext_frames[frame].linesize[plane] * line;
            st_test_rand_data(cur_line, least_line_size, frame);
          }
        }
      }
      if (tx_fmt[i] == ST_FRAME_FMT_YUV422PLANAR10LE) {
        /* only LSB 10 valid */
        uint16_t* p10_u16 = (uint16_t*)fb;
        for (size_t j = 0; j < (frame_size / 2); j++) {
          p10_u16[j] &= 0x3ff; /* only 10 bit */
        }
      } else if (tx_fmt[i] == ST_FRAME_FMT_Y210) {
        /* only MSB 10 valid */
        uint16_t* y210_u16 = (uint16_t*)fb;
        for (size_t j = 0; j < (frame_size / 2); j++) {
          y210_u16[j] &= 0xffc0; /* only 10 bit */
        }
      } else if (tx_fmt[i] == ST_FRAME_FMT_V210) {
        uint32_t* v210_word = (uint32_t*)fb;
        for (size_t j = 0; j < (frame_size / 4); j++) {
          v210_word[j] &= 0x3fffffff; /* only 30 bit */
        }
      }
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20p_tx", result);
      /* copy sha to the end of frame */
      if (para->device == ST_PLUGIN_DEVICE_TEST)
        memcpy(fb + frame_size - SHA256_DIGEST_LENGTH, result, SHA256_DIGEST_LENGTH);
    }

    test_ctx_tx[i]->handle = tx_handle[i];

    tx_thread[i] = std::thread(test_st20p_tx_frame_thread, test_ctx_tx[i]);
  }

  for (int i = 0; i < sessions; i++) {
    if (para->fail_interval) {
      /* loss in the tx */
      expect_framerate_tx[i] =
          expect_framerate_tx[i] * (para->fail_interval - 1) / para->fail_interval;
    }
    expect_framerate_rx[i] = expect_framerate_tx[i];
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->width = width[i];
    test_ctx_rx[i]->height = height[i];
    test_ctx_rx[i]->fmt = rx_fmt[i];
    test_ctx_rx[i]->user_timestamp = para->user_timestamp;
    test_ctx_rx[i]->user_meta = para->user_meta;
    test_ctx_rx[i]->block_get = para->block_get;
    test_ctx_rx[i]->rx_timing_parser = para->rx_timing_parser;
    test_ctx_rx[i]->frame_size =
        st_frame_size(rx_fmt[i], width[i], height[i], para->interlace);
    /* copy sha */
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    /* init ext frames, only for no convert */
    if (para->rx_ext) {
      uint8_t planes = st_frame_fmt_planes(rx_fmt[i]);
      test_ctx_rx[i]->p_ext_frames = (struct st_ext_frame*)malloc(
          sizeof(*test_ctx_rx[i]->p_ext_frames) * test_ctx_rx[i]->fb_cnt);
      size_t frame_size = st_frame_size(rx_fmt[i], width[i], height[i], para->interlace) +
                          para->line_padding_size * height[i] * planes;
      size_t pg_sz = mtl_page_size(st);
      size_t fb_size = frame_size * test_ctx_rx[i]->fb_cnt;
      test_ctx_rx[i]->ext_fb_iova_map_sz =
          mtl_size_page_align(fb_size, pg_sz); /* align */
      size_t fb_size_malloc = test_ctx_rx[i]->ext_fb_iova_map_sz + pg_sz;
      test_ctx_rx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
      ASSERT_TRUE(test_ctx_rx[i]->ext_fb_malloc != NULL);
      test_ctx_rx[i]->ext_fb =
          (uint8_t*)MTL_ALIGN((uint64_t)test_ctx_rx[i]->ext_fb_malloc, pg_sz);
      test_ctx_rx[i]->ext_fb_iova =
          mtl_dma_map(st, test_ctx_rx[i]->ext_fb, test_ctx_rx[i]->ext_fb_iova_map_sz);
      info("%s, session %d ext_fb %p\n", __func__, i, test_ctx_rx[i]->ext_fb);
      ASSERT_TRUE(test_ctx_rx[i]->ext_fb_iova != MTL_BAD_IOVA);

      for (int j = 0; j < test_ctx_rx[i]->fb_cnt; j++) {
        for (uint8_t plane = 0; plane < planes; plane++) { /* assume planes continuous */
          test_ctx_rx[i]->p_ext_frames[j].linesize[plane] =
              st_frame_least_linesize(rx_fmt[i], width[i], plane) +
              para->line_padding_size;
          if (plane == 0) {
            test_ctx_rx[i]->p_ext_frames[j].addr[plane] =
                test_ctx_rx[i]->ext_fb + j * frame_size;
            test_ctx_rx[i]->p_ext_frames[j].iova[plane] =
                test_ctx_rx[i]->ext_fb_iova + j * frame_size;
          } else {
            test_ctx_rx[i]->p_ext_frames[j].addr[plane] =
                (uint8_t*)test_ctx_rx[i]->p_ext_frames[j].addr[plane - 1] +
                test_ctx_rx[i]->p_ext_frames[j].linesize[plane - 1] * height[i];
            test_ctx_rx[i]->p_ext_frames[j].iova[plane] =
                test_ctx_rx[i]->p_ext_frames[j].iova[plane - 1] +
                test_ctx_rx[i]->p_ext_frames[j].linesize[plane - 1] * height[i];
          }
        }
        test_ctx_rx[i]->p_ext_frames[j].size = frame_size;
        test_ctx_rx[i]->p_ext_frames[j].opaque = NULL;
      }
      test_ctx_rx[i]->frame_size = frame_size;
    }

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20p_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.port.num_port = 1;
    if (ctx->mcast_only)
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    else
      memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx->para.sip_addr[MTL_PORT_P],
             MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ST20P_TEST_UDP_PORT + i * 2;
    ops_rx.port.payload_type = para->zero_payload_type ? 0 : ST20P_TEST_PAYLOAD_TYPE;
    ops_rx.port.ssrc = para->ssrc;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.output_fmt = rx_fmt[i];
    ops_rx.transport_fmt = t_fmt[i];
    ops_rx.interlaced = para->interlace;
    ops_rx.transport_linesize = 0;
    ops_rx.device = para->device;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    if (para->block_get)
      ops_rx.flags |= ST20P_RX_FLAG_BLOCK_GET;
    else
      ops_rx.notify_frame_available = test_st20p_rx_frame_available;
    if (para->rx_timing_parser) ops_rx.flags |= ST20P_RX_FLAG_TIMING_PARSER_META;
    ops_rx.notify_event = test_ctx_notify_event;
    if (para->rx_ext) {
      if (para->rx_dedicated_ext) {
        ops_rx.ext_frames = test_ctx_rx[i]->p_ext_frames;
      } else {
        ops_rx.flags |= ST20P_RX_FLAG_EXT_FRAME;
        ops_rx.query_ext_frame = test_st20p_rx_query_ext_frame;
        if (st_frame_fmt_equal_transport(ops_rx.output_fmt, ops_rx.transport_fmt))
          ops_rx.flags |= ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
      }
    }
    if (para->vsync) ops_rx.flags |= ST20P_RX_FLAG_ENABLE_VSYNC;
    if (para->pkt_convert) ops_rx.flags |= ST20P_RX_FLAG_PKT_CONVERT;
    if (para->rx_auto_detect) ops_rx.flags |= ST20P_RX_FLAG_AUTO_DETECT;

    if (para->rtcp) {
      ops_rx.flags |= ST20P_RX_FLAG_ENABLE_RTCP | ST20P_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 250;
      ops_rx.rtcp.seq_bitmap_size = 64;
      ops_rx.rtcp.seq_skip_window = 0;
      ops_rx.rtcp.burst_loss_max = 1;
      ops_rx.rtcp.sim_loss_rate = 0.1;
    }

    rx_handle[i] = st20p_rx_create(st, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    if (para->block_get) {
      ret = st20p_rx_set_block_timeout(rx_handle[i], NS_PER_S);
      EXPECT_EQ(ret, 0);
    }

    int sch = st20p_rx_get_sch_idx(rx_handle[i]);
    EXPECT_GE(sch, 0);
    ret = mtl_sch_enable_sleep(st, sch, false);
    EXPECT_GE(ret, 0);

    test_ctx_rx[i]->handle = rx_handle[i];

    if (para->device == ST_PLUGIN_DEVICE_TEST_INTERNAL)
      rx_thread[i] = std::thread(test_internal_st20p_rx_frame_thread, test_ctx_rx[i]);
    else
      rx_thread[i] = std::thread(test_st20p_rx_frame_thread, test_ctx_rx[i]);

    struct st_queue_meta meta;
    ret = st20p_rx_get_queue_meta(rx_handle[i], &meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(st);
  EXPECT_GE(ret, 0);
  sleep(10);
  if (!para->send_done_check) {
    ret = mtl_stop(st);
    EXPECT_GE(ret, 0);
  }

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_tx[i]->start_time) / NS_PER_S;
    framerate_tx[i] = test_ctx_tx[i]->fb_send / time_sec;

    /* vsync check */
    time_sec = (double)(cur_time_ns - test_ctx_tx[i]->first_vsync_time) / NS_PER_S;
    vsyncrate_tx[i] = test_ctx_tx[i]->vsync_cnt / time_sec;
    dbg("%s(%d,%p), vsync_cnt %d vsyncrate %f\n", __func__, i, test_ctx_tx[i],
        test_ctx_tx[i]->vsync_cnt, vsyncrate_tx[i]);
    EXPECT_GT(test_ctx_tx[i]->vsync_cnt, 0);
    EXPECT_NEAR(vsyncrate_tx[i], st_frame_rate(fps[i]), st_frame_rate(fps[i]) * 0.1);

    test_ctx_tx[i]->stop = true;
    if (para->block_get) st20p_tx_wake_block(tx_handle[i]);
    test_ctx_tx[i]->cv.notify_all();
    tx_thread[i].join();
    if (para->send_done_check) {
      st_usleep(1000 * 100); /* wait all fb done */
      EXPECT_EQ(test_ctx_tx[i]->fb_send, test_ctx_tx[i]->fb_send_done);
    }
  }
  if (para->send_done_check) {
    ret = mtl_stop(st);
    EXPECT_GE(ret, 0);
  }
  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate_rx[i] = test_ctx_rx[i]->fb_rec / time_sec;

    /* vsync check */
    time_sec = (double)(cur_time_ns - test_ctx_rx[i]->first_vsync_time) / NS_PER_S;
    vsyncrate_rx[i] = test_ctx_rx[i]->vsync_cnt / time_sec;
    dbg("%s(%d,%p), vsync_cnt %d vsyncrate %f\n", __func__, i, test_ctx_rx[i],
        test_ctx_rx[i]->vsync_cnt, vsyncrate_rx[i]);
    EXPECT_GT(test_ctx_rx[i]->vsync_cnt, 0);

    /* with kernel:lo interfaces we don't have enough single core performance to
     * perform this test */
    if ((strcmp(ctx->para.port[MTL_PORT_P], "kernel:lo") != 0) &&
        (strcmp(ctx->para.port[MTL_PORT_R], "kernel:lo") != 0))
      EXPECT_NEAR(vsyncrate_rx[i], st_frame_rate(fps[i]), st_frame_rate(fps[i]) * 0.1);
    else
      info("%s, skip vsync check as it's kernel:lo\n", __func__);

    test_ctx_rx[i]->stop = true;
    if (para->block_get) st20p_rx_wake_block(rx_handle[i]);
    test_ctx_rx[i]->cv.notify_all();
    rx_thread[i].join();
  }

  for (int i = 0; i < sessions; i++) {
    ret = st20p_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    info("%s, session %d fb_send %d framerate %f:%f\n", __func__, i,
         test_ctx_tx[i]->fb_send, framerate_tx[i], expect_framerate_tx[i]);
    EXPECT_GT(test_ctx_tx[i]->fb_send, 0);
    if (para->tx_ext) {
      mtl_dma_unmap(st, test_ctx_tx[i]->ext_fb, test_ctx_tx[i]->ext_fb_iova,
                    test_ctx_tx[i]->ext_fb_iova_map_sz);
      st_test_free(test_ctx_tx[i]->ext_fb_malloc);
      st_test_free(test_ctx_tx[i]->p_ext_frames);
    }
    test_ctx_rx[i]->fb_send = test_ctx_tx[i]->fb_send;
    delete test_ctx_tx[i];
    test_ctx_tx[i] = NULL;
  }
  for (int i = 0; i < sessions; i++) {
    ret = st20p_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    info("%s, session %d fb_rec %d framerate %f:%f\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate_rx[i], expect_framerate_rx[i]);
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_LE(test_ctx_rx[i]->incomplete_frame_cnt, 4);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    EXPECT_LE(test_ctx_rx[i]->user_meta_fail_cnt, 2);
    if (para->check_fps) {
      if (para->fail_interval || para->timeout_interval) {
        EXPECT_NEAR(framerate_rx[i], expect_framerate_rx[i],
                    expect_framerate_rx[i] * 0.5);
      } else {
        EXPECT_NEAR(framerate_rx[i], expect_framerate_rx[i],
                    expect_framerate_rx[i] * 0.1);
      }
    }
    if (para->rx_ext) {
      mtl_dma_unmap(st, test_ctx_rx[i]->ext_fb, test_ctx_rx[i]->ext_fb_iova,
                    test_ctx_rx[i]->ext_fb_iova_map_sz);
      st_test_free(test_ctx_rx[i]->ext_fb_malloc);
      st_test_free(test_ctx_rx[i]->p_ext_frames);
    }
    if (para->rtcp) {
      info("%s, session %d rx/tx fb ratio %f\n", __func__, i,
           (double)test_ctx_rx[i]->fb_rec / test_ctx_rx[i]->fb_send);
    }
    delete test_ctx_rx[i];
    test_ctx_rx[i] = NULL;
  }
}

TEST(St20p, digest_1080p_s1) {
  enum st_fps fps[1] = {ST_FPS_P25};
  int width[1] = {1280};
  int height[1] = {720};
  enum st_frame_fmt tx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st20_fmt t_fmt[1] = {ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.level = ST_TEST_LEVEL_MANDATORY;
  para.check_fps = false;
  para.rx_timing_parser = true;
  para.rx_auto_detect = true;
  para.zero_payload_type = true;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_1080i_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422PLANAR10LE};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.interlace = true;
  para.level = ST_TEST_LEVEL_MANDATORY;
  para.check_fps = false;
  para.interlace = true;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.ssrc = 54321;
  para.block_get = true;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_1080p_internal_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt tx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st20_fmt t_fmt[1] = {ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.level = ST_TEST_LEVEL_ALL;
  para.check_fps = false;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE,
                                 ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE,
                                 ST_FRAME_FMT_YUV422PLANAR10LE};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.level = ST_TEST_LEVEL_ALL;
  para.check_fps = false;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_1080p_fail_interval) {
  enum st_fps fps[1] = {ST_FPS_P25};
  int width[1] = {1280};
  int height[1] = {720};
  enum st_frame_fmt tx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st20_fmt t_fmt[1] = {ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.fail_interval = 3;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_1080p_timeout_interval) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt tx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st20_fmt t_fmt[1] = {ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.timeout_interval = 3;
  para.timeout_ms = 20;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_1080p_internal_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.check_fps = false;
  para.level = ST_TEST_LEVEL_ALL;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_1080p_no_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST_FRAME_FMT_RGB8};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_RGB_8BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST_FRAME_FMT_RGB8};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.level = ST_TEST_LEVEL_ALL;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_1080p_packet_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.check_fps = false;
  para.pkt_convert = true;
  para.send_done_check = false;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, tx_ext_digest_1080p_no_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422RFC4175PG2BE10};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422RFC4175PG2BE10};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.tx_ext = true;
  para.level = ST_TEST_LEVEL_ALL;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, tx_ext_digest_1080p_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_V210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_V210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.tx_ext = true;
  para.level = ST_TEST_LEVEL_ALL;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, rx_ext_digest_1080p_no_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422RFC4175PG2BE10};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422RFC4175PG2BE10};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.rx_ext = true;
  para.block_get = true;
  para.rx_auto_detect = true;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, rx_ext_digest_1080p_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_V210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_V210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.rx_ext = true;
  para.check_fps = false;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, rx_ext_digest_1080p_packet_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.rx_ext = true;
  para.check_fps = false;
  para.pkt_convert = true;
  para.level = ST_TEST_LEVEL_ALL;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, ext_digest_1080p_no_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422RFC4175PG2BE10};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422RFC4175PG2BE10};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.tx_ext = true;
  para.rx_ext = true;
  para.check_fps = false;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, ext_digest_1080p_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_V210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_V210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.tx_ext = true;
  para.rx_ext = true;
  para.check_fps = false;
  para.user_timestamp = true;
  para.block_get = false;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, rx_dedicated_ext_digest_1080p_convert_s2) {
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P59_94};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.rx_ext = true;
  para.rx_dedicated_ext = true;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, ext_digest_1080p_convert_with_padding_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.tx_ext = true;
  para.rx_ext = true;
  para.check_fps = false;
  para.line_padding_size = 1024;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, rx_dedicated_ext_digest_1080p_convert_with_padding_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.tx_ext = true;
  para.rx_ext = true;
  para.rx_dedicated_ext = true;
  para.check_fps = false;
  para.line_padding_size = 512;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, ext_digest_1080p_packet_convert_with_padding_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.tx_ext = true;
  para.rx_ext = true;
  para.rx_dedicated_ext = true;
  para.check_fps = false;
  para.line_padding_size = 1024;
  para.pkt_convert = true;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_user_meta_s2) {
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt tx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st20_fmt t_fmt[2] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[2] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                 ST_FRAME_FMT_YUV422PLANAR10LE};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.device = ST_PLUGIN_DEVICE_TEST_INTERNAL;
  para.level = ST_TEST_LEVEL_MANDATORY;
  para.user_meta = true;
  para.check_fps = false;
  para.packing = ST20_PACKING_GPM;
  para.block_get = true;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, digest_rtcp_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt tx_fmt[1] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10};
  enum st20_fmt t_fmt[1] = {ST20_FMT_YUV_422_10BIT};
  enum st_frame_fmt rx_fmt[1] = {ST_FRAME_FMT_YUV422RFC4175PG2BE10};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.level = ST_TEST_LEVEL_MANDATORY;
  para.rtcp = true;
  para.check_fps = false;
  para.packing = ST20_PACKING_GPM_SL;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}

TEST(St20p, transport_yuv422p10le) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt tx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st20_fmt t_fmt[1] = {ST20_FMT_YUV_422_PLANAR10LE};
  enum st_frame_fmt rx_fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};

  struct st20p_rx_digest_test_para para;
  test_st20p_init_rx_digest_para(&para);
  para.level = ST_TEST_LEVEL_ALL;
  para.packing = ST20_PACKING_BPM;

  st20p_rx_digest_test(fps, width, height, tx_fmt, t_fmt, rx_fmt, &para);
}
