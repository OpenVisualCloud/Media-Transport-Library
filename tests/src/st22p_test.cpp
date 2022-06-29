/*
 * Copyright (C) 2022 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include <thread>

#include "log.h"
#include "tests.h"

#define ST22P_TEST_PAYLOAD_TYPE (114)
#define ST22P_TEST_UDP_PORT (16000)

static int test_encode_frame(struct jpegxs_encoder_session* s,
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
  memcpy(frame->dst->addr,
         (uint8_t*)frame->src->addr + frame->src->data_size - SHA256_DIGEST_LENGTH,
         SHA256_DIGEST_LENGTH);
  usleep(s->sleep_time_us);
  /* data size indicate the encode stream size for current frame */
  frame->dst->data_size = codestream_size;

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
      usleep(s->timeout_ms * 1000);
    }
  }

  return 0;
}

static void* test_encode_thread(void* arg) {
  struct jpegxs_encoder_session* s = (struct jpegxs_encoder_session*)arg;
  st22p_encode_session session_p = s->session_p;
  struct st22_encode_frame_meta* frame;
  int result;

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22_encoder_get_frame(session_p);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
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
  struct jpegxs_encoder_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_SAMPLE_ENCODER_SESSIONS; i++) {
    if (ctx->encoder_sessions[i]) continue;
    session = (struct jpegxs_encoder_session*)malloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->idx = i;
    st_pthread_mutex_init(&session->wake_mutex, NULL);
    st_pthread_cond_init(&session->wake_cond, NULL);

    req->max_codestream_size = req->codestream_size;

    session->req = *req;
    session->session_p = session_p;
    double fps = st_frame_rate(req->fps);
    if (!fps) fps = 60;
    session->sleep_time_us = 1000 * 1000 / fps / 2;
    dbg("%s(%d), sleep_time_us %d\n", __func__, i, session->sleep_time_us);
    session->fail_interval = ctx->jpegxs_fail_interval;
    session->timeout_interval = ctx->jpegxs_timeout_interval;
    session->timeout_ms = ctx->jpegxs_timeout_ms;

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
    dbg("%s(%d), frame_max_size %ld\n", __func__, i, frame_max_size);
    return session;
  }

  dbg("%s, all session slot are used\n", __func__);
  return NULL;
}

static int test_encoder_free_session(void* priv, st22_encode_priv session) {
  struct st_tests_context* ctx = (struct st_tests_context*)priv;
  struct jpegxs_encoder_session* encoder_session =
      (struct jpegxs_encoder_session*)session;
  int idx = encoder_session->idx;

  encoder_session->stop = true;
  st_pthread_mutex_lock(&encoder_session->wake_mutex);
  st_pthread_cond_signal(&encoder_session->wake_cond);
  st_pthread_mutex_unlock(&encoder_session->wake_mutex);
  pthread_join(encoder_session->encode_thread, NULL);

  st_pthread_mutex_destroy(&encoder_session->wake_mutex);
  st_pthread_cond_destroy(&encoder_session->wake_cond);

  dbg("%s(%d), total %d encode frames\n", __func__, idx, encoder_session->frame_cnt);
  free(encoder_session);
  ctx->encoder_sessions[idx] = NULL;
  return 0;
}

static int test_encoder_frame_available(void* priv) {
  struct jpegxs_encoder_session* s = (struct jpegxs_encoder_session*)priv;

  // dbg("%s(%d)\n", __func__, s->idx);
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int test_decode_frame(struct jpegxs_decoder_session* s,
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
  memcpy((uint8_t*)frame->dst->addr + frame->dst->data_size - SHA256_DIGEST_LENGTH,
         frame->src->addr, SHA256_DIGEST_LENGTH);
  usleep(s->sleep_time_us);

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
      usleep(s->timeout_ms * 1000);
    }
  }

  return 0;
}

static void* test_decode_thread(void* arg) {
  struct jpegxs_decoder_session* s = (struct jpegxs_decoder_session*)arg;
  st22p_decode_session session_p = s->session_p;
  struct st22_decode_frame_meta* frame;
  int result;

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22_decoder_get_frame(session_p);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
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
  struct jpegxs_decoder_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_SAMPLE_DECODER_SESSIONS; i++) {
    if (ctx->decoder_sessions[i]) continue;
    session = (struct jpegxs_decoder_session*)malloc(sizeof(*session));
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
    session->fail_interval = ctx->jpegxs_fail_interval;
    session->timeout_interval = ctx->jpegxs_timeout_interval;
    session->timeout_ms = ctx->jpegxs_timeout_ms;

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
  struct jpegxs_decoder_session* decoder_session =
      (struct jpegxs_decoder_session*)session;
  int idx = decoder_session->idx;

  decoder_session->stop = true;
  st_pthread_mutex_lock(&decoder_session->wake_mutex);
  st_pthread_cond_signal(&decoder_session->wake_cond);
  st_pthread_mutex_unlock(&decoder_session->wake_mutex);
  pthread_join(decoder_session->decode_thread, NULL);

  st_pthread_mutex_destroy(&decoder_session->wake_mutex);
  st_pthread_cond_destroy(&decoder_session->wake_cond);

  dbg("%s(%d), total %d decode frames\n", __func__, idx, decoder_session->frame_cnt);
  free(decoder_session);
  ctx->decoder_sessions[idx] = NULL;
  return 0;
}

static int test_decoder_frame_available(void* priv) {
  struct jpegxs_decoder_session* s = (struct jpegxs_decoder_session*)priv;

  // dbg("%s(%d)\n", __func__, s->idx);
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

int st_test_jpegxs_plugin_unregister(struct st_tests_context* ctx) {
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

int st_test_jpegxs_plugin_register(struct st_tests_context* ctx) {
  auto st = ctx->handle;
  int ret = 0;

  struct st22_decoder_dev d_dev;
  memset(&d_dev, 0, sizeof(d_dev));
  d_dev.name = "jpegxs_test_decoder";
  d_dev.priv = ctx;
  d_dev.codec = ST22_CODEC_JPEGXS;
  d_dev.target_device = ST_PLUGIN_DEVICE_TEST;
  d_dev.input_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM;
  d_dev.output_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE;
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
  e_dev.name = "jpegxs_test_encoder";
  e_dev.priv = ctx;
  e_dev.codec = ST22_CODEC_JPEGXS;
  e_dev.target_device = ST_PLUGIN_DEVICE_TEST;
  e_dev.input_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE;
  e_dev.output_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM;
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

  if (ret < 0) {
    EXPECT_EQ(pre_nb, new_nb);
  } else {
    EXPECT_EQ(pre_nb + 1, new_nb);
  }
}

TEST(St22p, plugin_register_single) {
  plugin_register_test("/usr/local/lib/x86_64-linux-gnu/libst_plugin_sample.so", true);
}
TEST(St22p, plugin_register_fail) {
  plugin_register_test("/usr/local/lib/x86_64-linux-gnu/libst_plugin_sample_fail.so",
                       false);
}

static void frame_size_test() {
  uint32_t w = 1920;
  uint32_t h = 1080;
  size_t size;

  size = st_frame_size(ST_FRAME_FMT_YUV422PLANAR10LE, w, h);
  EXPECT_GT(size, 0);
  size = st_frame_size(ST_FRAME_FMT_V210, w, h);
  EXPECT_GT(size, 0);
  size = st_frame_size(ST_FRAME_FMT_YUV422PLANAR8, w, h);
  EXPECT_GT(size, 0);
  size = st_frame_size(ST_FRAME_FMT_YUV422PACKED8, w, h);
  EXPECT_GT(size, 0);
  size = st_frame_size(ST_FRAME_FMT_YUV422RFC4175PG2BE10, w, h);
  EXPECT_GT(size, 0);

  size = st_frame_size(ST_FRAME_FMT_ARGB, w, h);
  EXPECT_GT(size, 0);
  size = st_frame_size(ST_FRAME_FMT_BGRA, w, h);
  EXPECT_GT(size, 0);
  size = st_frame_size(ST_FRAME_FMT_RGB8, w, h);
  EXPECT_GT(size, 0);

  size = st_frame_size(ST_FRAME_FMT_MAX, w, h);
  EXPECT_EQ(size, 0);
}

static void frame_name_test() {
  int result;
  const char* fail = "unknown";

  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_YUV422PLANAR10LE));
  EXPECT_NE(result, 0);
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_V210));
  EXPECT_NE(result, 0);
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_YUV422PLANAR8));
  EXPECT_NE(result, 0);
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_YUV422PACKED8));
  EXPECT_NE(result, 0);
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_YUV422RFC4175PG2BE10));
  EXPECT_NE(result, 0);

  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_ARGB));
  EXPECT_NE(result, 0);
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_BGRA));
  EXPECT_NE(result, 0);
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_RGB8));
  EXPECT_NE(result, 0);

  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_MAX));
  EXPECT_EQ(result, 0);
}

TEST(St22p, frame_size) { frame_size_test(); }
TEST(St22p, frame_name) { frame_name_test(); }

static void frame_draw_logo_test(enum st_frame_fmt fmt, uint32_t w, uint32_t h,
                                 uint32_t logo_w, uint32_t logo_h, uint32_t x, uint32_t y,
                                 bool expect) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  size_t logo_size = st_frame_size(fmt, logo_w, logo_h);
  size_t frame_size = st_frame_size(fmt, w, h);
  void* frame_buf = st_hp_malloc(st, frame_size, ST_PORT_P);
  if (!frame_buf) {
    err("%s, frame_buf malloc fail\n", __func__);
    return;
  }
  void* logo_buf = st_hp_malloc(st, logo_size, ST_PORT_P);
  if (!logo_buf) {
    err("%s, logo_buf malloc fail\n", __func__);
    st_hp_free(st, frame_buf);
    return;
  }

  struct st_frame_meta frame_meta;
  struct st_frame_meta logo_meta;
  frame_meta.addr = frame_buf;
  frame_meta.fmt = fmt;
  frame_meta.width = w;
  frame_meta.height = h;
  logo_meta.addr = logo_buf;
  logo_meta.fmt = fmt;
  logo_meta.width = logo_w;
  logo_meta.height = logo_h;

  int ret = st_draw_logo(&frame_meta, &logo_meta, x, y);
  if (expect)
    EXPECT_GE(ret, 0);
  else
    EXPECT_LT(ret, 0);

  st_hp_free(st, logo_buf);
  st_hp_free(st, frame_buf);
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

  s->cv.notify_all();

  return 0;
}

static int test_st22p_rx_frame_available(void* priv) {
  tests_context* s = (tests_context*)priv;

  s->cv.notify_all();

  return 0;
}

static void st22p_tx_ops_init(tests_context* st22, struct st22p_tx_ops* ops_tx) {
  auto ctx = st22->ctx;

  memset(ops_tx, 0, sizeof(*ops_tx));
  ops_tx->name = "st22p_test";
  ops_tx->priv = st22;
  ops_tx->port.num_port = 1;
  memcpy(ops_tx->port.dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops_tx->port.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops_tx->port.udp_port[ST_PORT_P] = ST22P_TEST_UDP_PORT + st22->idx;
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
  st22->frame_size = st_frame_size(ops_tx->input_fmt, ops_tx->width, ops_tx->height);
  ops_tx->codestream_size = st22->frame_size / 8;
}

static void st22p_rx_ops_init(tests_context* st22, struct st22p_rx_ops* ops_rx) {
  auto ctx = st22->ctx;

  memset(ops_rx, 0, sizeof(*ops_rx));
  ops_rx->name = "st22p_test";
  ops_rx->priv = st22;
  ops_rx->port.num_port = 1;
  memcpy(ops_rx->port.sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops_rx->port.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
  ops_rx->port.udp_port[ST_PORT_P] = ST22P_TEST_UDP_PORT + st22->idx;
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
  st22->frame_size = st_frame_size(ops_rx->output_fmt, ops_rx->width, ops_rx->height);
}

static void st22p_tx_assert_cnt(int expect_s22_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st22_tx_sessions_cnt, expect_s22_tx_cnt);
}

static void st22p_rx_assert_cnt(int expect_s22_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st22_rx_sessions_cnt, expect_s22_rx_cnt);
}

TEST(St22p, tx_create_free_single) { pipeline_create_free_test(st22p_tx, 0, 1, 1); }
TEST(St22p, tx_create_free_multi) { pipeline_create_free_test(st22p_tx, 0, 1, 6); }
TEST(St22p, tx_create_free_mix) { pipeline_create_free_test(st22p_tx, 2, 3, 4); }
TEST(St22p, rx_create_free_single) { pipeline_create_free_test(st22p_rx, 0, 1, 1); }
TEST(St22p, rx_create_free_multi) { pipeline_create_free_test(st22p_rx, 0, 1, 6); }
TEST(St22p, rx_create_free_mix) { pipeline_create_free_test(st22p_rx, 2, 3, 4); }
TEST(St22p, tx_create_free_max) { pipeline_create_free_max(st22p_tx, 100); }
TEST(St22p, rx_create_free_max) { pipeline_create_free_max(st22p_rx, 100); }
TEST(St22p, tx_create_expect_fail) { pipeline_expect_fail_test(st22p_tx); }
TEST(St22p, rx_create_expect_fail) { pipeline_expect_fail_test(st22p_rx); }
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
  auto handle = s->handle;
  struct st_frame_meta* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22p_tx_get_frame((st22p_tx_handle)handle);
    if (!frame) { /* no frame */
      lck.lock();
      if (!s->stop) s->cv.wait(lck);
      lck.unlock();
      continue;
    }
    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->width != s->width) s->incomplete_frame_cnt++;
    if (frame->height != s->height) s->incomplete_frame_cnt++;
    if (frame->fmt != s->fmt) s->incomplete_frame_cnt++;
    /* directly put */
    st22p_tx_put_frame((st22p_tx_handle)handle, frame);
    s->fb_send++;
    if (!s->start_time) {
      s->start_time = st_test_get_monotonic_time();
      dbg("%s(%d), start_time %ld\n", __func__, s->idx, s->start_time);
    }
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void test_st22p_rx_frame_thread(void* args) {
  tests_context* s = (tests_context*)args;
  auto handle = s->handle;
  struct st_frame_meta* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);
  uint64_t timestamp = 0;

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22p_rx_get_frame((st22p_rx_handle)handle);
    if (!frame) { /* no frame */
      lck.lock();
      if (!s->stop) s->cv.wait(lck);
      lck.unlock();
      continue;
    }

    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->width != s->width) s->incomplete_frame_cnt++;
    if (frame->height != s->height) s->incomplete_frame_cnt++;
    if (frame->fmt != s->fmt) s->incomplete_frame_cnt++;
    dbg("%s(%d), timestamp %ld\n", __func__, s->idx, frame->timestamp);
    if (frame->timestamp == timestamp) s->incomplete_frame_cnt++;
    timestamp = frame->timestamp;

    unsigned char* sha =
        (unsigned char*)frame->addr + frame->data_size - SHA256_DIGEST_LENGTH;
    int i = 0;
    for (i = 0; i < TEST_SHA_HIST_NUM; i++) {
      unsigned char* target_sha = s->shas[i];
      if (!memcmp(sha, target_sha, SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= TEST_SHA_HIST_NUM) {
      test_sha_dump("st22p_rx_error_sha", sha);
      s->fail_cnt++;
    }
    /* directly put */
    st22p_rx_put_frame((st22p_rx_handle)handle, frame);
    s->fb_rec++;
    if (!s->start_time) s->start_time = st_test_get_monotonic_time();
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void st22p_rx_digest_test(enum st_fps fps[], int width[], int height[],
                                 enum st_frame_fmt fmt[], enum st22_codec codec[],
                                 int compress_ratio[], int sessions = 1,
                                 int fail_interval = 0, int timeout_interval = 0,
                                 int timeout_ms = 0) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  int ret;
  struct st22p_tx_ops ops_tx;
  struct st22p_rx_ops ops_rx;

  st_test_jxs_fail_interval(ctx, fail_interval);
  st_test_jxs_timeout_interval(ctx, timeout_interval);
  st_test_jxs_timeout_ms(ctx, timeout_ms);

  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled, one for tx and one for rx\n", __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22p_tx_handle> tx_handle;
  std::vector<st22p_rx_handle> rx_handle;
  std::vector<double> expect_framerate_tx;
  std::vector<double> expect_framerate_rx;
  std::vector<double> framerate_tx;
  std::vector<double> framerate_rx;
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
  tx_thread.resize(sessions);
  rx_thread.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate_tx[i] = st_frame_rate(fps[i]);
    if (timeout_interval) {
      expect_framerate_tx[i] =
          expect_framerate_tx[i] * (timeout_interval - 1) / timeout_interval;
    }

    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->width = width[i];
    test_ctx_tx[i]->height = height[i];
    test_ctx_tx[i]->fmt = fmt[i];

    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22p_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.port.num_port = 1;
    memcpy(ops_tx.port.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R],
           ST_IP_ADDR_LEN);
    strncpy(ops_tx.port.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.port.udp_port[ST_PORT_P] = ST22P_TEST_UDP_PORT + i;
    ops_tx.port.payload_type = ST22P_TEST_PAYLOAD_TYPE;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.input_fmt = fmt[i];
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.codec = codec[i];
    ops_tx.device = ST_PLUGIN_DEVICE_TEST;
    ops_tx.quality = ST22_QUALITY_MODE_QUALITY;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.notify_frame_available = test_st22p_tx_frame_available;

    test_ctx_tx[i]->frame_size =
        st_frame_size(ops_tx.input_fmt, ops_tx.width, ops_tx.height);

    ops_tx.codestream_size = test_ctx_tx[i]->frame_size / compress_ratio[i];

    tx_handle[i] = st22p_tx_create(st, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha caculate */
    size_t frame_size = test_ctx_tx[i]->frame_size;
    uint8_t* fb;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      fb = (uint8_t*)st22p_tx_get_fb_addr(tx_handle[i], frame);
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st22p_tx", result);
      /* copy sha to the end of frame */
      memcpy(fb + frame_size - SHA256_DIGEST_LENGTH, result, SHA256_DIGEST_LENGTH);
    }

    test_ctx_tx[i]->handle = tx_handle[i];

    tx_thread[i] = std::thread(test_st22p_tx_frame_thread, test_ctx_tx[i]);
  }

  for (int i = 0; i < sessions; i++) {
    expect_framerate_rx[i] = expect_framerate_tx[i];
    if (fail_interval) {
      /* loss in the tx */
      expect_framerate_rx[i] =
          expect_framerate_rx[i] * (fail_interval - 1) / fail_interval;
      /* loss in the rx */
      expect_framerate_rx[i] =
          expect_framerate_rx[i] * (fail_interval - 1) / fail_interval;
    }
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = TEST_SHA_HIST_NUM;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->width = width[i];
    test_ctx_rx[i]->height = height[i];
    test_ctx_rx[i]->fmt = fmt[i];
    /* copy sha */
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22p_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.port.num_port = 1;
    memcpy(ops_rx.port.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P],
           ST_IP_ADDR_LEN);
    strncpy(ops_rx.port.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.port.udp_port[ST_PORT_P] = ST22P_TEST_UDP_PORT + i;
    ops_rx.port.payload_type = ST22P_TEST_PAYLOAD_TYPE;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.output_fmt = fmt[i];
    ops_rx.pack_type = ST22_PACK_CODESTREAM;
    ops_rx.codec = codec[i];
    ops_rx.device = ST_PLUGIN_DEVICE_TEST;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_available = test_st22p_rx_frame_available;

    test_ctx_rx[i]->frame_size =
        st_frame_size(ops_rx.output_fmt, ops_rx.width, ops_rx.height);

    rx_handle[i] = st22p_rx_create(st, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->handle = rx_handle[i];

    rx_thread[i] = std::thread(test_st22p_rx_frame_thread, test_ctx_rx[i]);
  }

  ret = st_start(st);
  EXPECT_GE(ret, 0);
  sleep(10);
  ret = st_stop(st);
  EXPECT_GE(ret, 0);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_tx[i]->start_time) / NS_PER_S;
    framerate_tx[i] = test_ctx_tx[i]->fb_send / time_sec;

    test_ctx_tx[i]->stop = true;
    test_ctx_tx[i]->cv.notify_all();
    tx_thread[i].join();
  }
  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate_rx[i] = test_ctx_rx[i]->fb_rec / time_sec;

    test_ctx_rx[i]->stop = true;
    test_ctx_rx[i]->cv.notify_all();
    rx_thread[i].join();
  }

  for (int i = 0; i < sessions; i++) {
    ret = st22p_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    dbg("%s, session %d fb_send %d framerate %f:%f\n", __func__, i,
        test_ctx_tx[i]->fb_send, framerate_tx[i], expect_framerate_tx[i]);
    EXPECT_GE(test_ctx_rx[i]->fb_send, 0);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_frame_cnt, 0);
    delete test_ctx_tx[i];
  }
  for (int i = 0; i < sessions; i++) {
    ret = st22p_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    info("%s, session %d fb_rec %d framerate %f:%f\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate_rx[i], expect_framerate_rx[i]);
    EXPECT_GE(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_frame_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->fail_cnt, 0);
    if (fail_interval || timeout_interval) {
      EXPECT_NEAR(framerate_rx[i], expect_framerate_rx[i], expect_framerate_rx[i] * 0.5);
    } else {
      EXPECT_NEAR(framerate_rx[i], expect_framerate_rx[i], expect_framerate_rx[i] * 0.1);
    }
    delete test_ctx_rx[i];
  }
}

TEST(St22p, digest_jpegxs_1080p_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio);
}

TEST(St22p, digest_jpegxs_4k_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 2};
  int height[1] = {1080 * 2};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {20};

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio);
}

TEST(St22p, digest_jpegxs_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE,
                              ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[2] = {ST22_CODEC_JPEGXS, ST22_CODEC_JPEGXS};
  int compress_ratio[2] = {10, 16};

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, 2);
}

TEST(St22p, digest_jpegxs_1080p_fail_interval) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, 1, 3, 0, 0);
}

TEST(St22p, digest_jpegxs_1080p_timeout_interval) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, 1, 0, 3, 20);
}
