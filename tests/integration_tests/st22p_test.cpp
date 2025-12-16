/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <thread>

#include "log.h"
#include "tests.hpp"

#define ST22P_TEST_PAYLOAD_TYPE (114)
#define ST22P_TEST_UDP_PORT (16000)

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
    dbg("%s(%d), frame_max_size %" PRIu64 "\n", __func__, i, frame_max_size);
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

  // dbg("%s(%d)\n", __func__, s->idx);
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

static void* test_decode_thread(void* arg) {
  struct test_st22_decoder_session* s = (struct test_st22_decoder_session*)arg;
  st22p_decode_session session_p = s->session_p;
  struct st_tests_context* ctx = s->ctx;
  struct st22_decode_frame_meta* frame;
  int result;

  if (ctx->encoder_use_block_get) st22_decoder_set_block_timeout(session_p, NS_PER_S);

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
  if (ctx->encoder_use_block_get) {
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

  // dbg("%s(%d)\n", __func__, s->idx);
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

static void frame_draw_logo_test(enum st_frame_fmt fmt, uint32_t w, uint32_t h,
                                 uint32_t logo_w, uint32_t logo_h, uint32_t x, uint32_t y,
                                 bool expect) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  size_t logo_size = st_frame_size(fmt, logo_w, logo_h, false);
  size_t frame_size = st_frame_size(fmt, w, h, false);
  void* frame_buf = mtl_hp_malloc(st, frame_size, MTL_PORT_P);
  if (!frame_buf) {
    err("%s, frame_buf malloc fail\n", __func__);
    return;
  }
  void* logo_buf = mtl_hp_malloc(st, logo_size, MTL_PORT_P);
  if (!logo_buf) {
    err("%s, logo_buf malloc fail\n", __func__);
    mtl_hp_free(st, frame_buf);
    return;
  }

  struct st_frame frame_meta;
  struct st_frame logo_meta;
  frame_meta.addr[0] = frame_buf;
  frame_meta.fmt = fmt;
  frame_meta.width = w;
  frame_meta.height = h;
  logo_meta.addr[0] = logo_buf;
  logo_meta.fmt = fmt;
  logo_meta.width = logo_w;
  logo_meta.height = logo_h;

  int ret = st_draw_logo(&frame_meta, &logo_meta, x, y);
  if (expect)
    EXPECT_GE(ret, 0);
  else
    EXPECT_LT(ret, 0);

  mtl_hp_free(st, logo_buf);
  mtl_hp_free(st, frame_buf);
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

static int test_st22p_tx_frame_done(void* priv, struct st_frame* frame) {
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

static int test_st22p_rx_frame_available(void* priv) {
  tests_context* s = (tests_context*)priv;

  s->cv.notify_all();

  return 0;
}

static int test_st22p_rx_query_ext_frame(void* priv, st_ext_frame* ext_frame,
                                         struct st22_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  if (!ctx->handle) return -EIO; /* not ready */
  int i = ctx->ext_idx;

  /* check ext_fb_in_use */
  if (ctx->ext_fb_in_use[i]) {
    err("%s(%d), ext frame %d in use\n", __func__, ctx->idx, i);
    return -EIO;
  }
  *ext_frame = ctx->p_ext_frames[i];

  dbg("%s(%d), set ext frame %d(%p) to use\n", __func__, ctx->idx, i, ext_frame->addr);
  ctx->ext_fb_in_use[i] = true;

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
  auto handle = s->handle;
  struct st_frame* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22p_tx_get_frame((st22p_tx_handle)handle);
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
      frame->timestamp = s->fb_send + 1; /* add one to avoid zero timestamp */
      dbg("%s(%d), timestamp %d\n", __func__, s->idx, s->fb_send);
    }
    if (s->p_ext_frames) {
      int ret = st22p_tx_put_ext_frame((st22p_tx_handle)handle, frame,
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
      st22p_tx_put_frame((st22p_tx_handle)handle, frame);
    }
    s->fb_send++;
    if (!s->start_time) {
      s->start_time = st_test_get_monotonic_time();
      dbg("%s(%d), start_time %" PRIu64 "\n", __func__, s->idx, s->start_time);
    }
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

static void test_st22p_rx_frame_thread(void* args) {
  tests_context* s = (tests_context*)args;
  auto handle = s->handle;
  struct st_frame* frame;
  std::unique_lock<std::mutex> lck(s->mtx, std::defer_lock);

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22p_rx_get_frame((st22p_rx_handle)handle);
    if (!frame) { /* no frame */
      if (!s->block_get) {
        lck.lock();
        if (!s->stop) s->cv.wait(lck);
        lck.unlock();
      }
      continue;
    }

    if (frame->data_size != s->frame_size) s->incomplete_frame_cnt++;
    if (frame->buffer_size < s->frame_size) s->incomplete_frame_cnt++;
    if (frame->width != s->width) s->incomplete_frame_cnt++;
    if (frame->height != s->height) s->incomplete_frame_cnt++;
    if (frame->fmt != s->fmt) s->incomplete_frame_cnt++;
    dbg("%s(%d), timestamp %" PRIu64 "\n", __func__, s->idx, frame->timestamp);
    if (frame->timestamp == 0) s->incomplete_frame_cnt++;

    if (frame->opaque) {
      /* free dynamic ext frame */
      bool* in_use = (bool*)frame->opaque;
      EXPECT_TRUE(*in_use);
      *in_use = false;
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
    for (i = 0; i < ST22_TEST_SHA_HIST_NUM; i++) {
      unsigned char* target_sha = s->shas[i];
      if (!memcmp(sha, target_sha, SHA256_DIGEST_LENGTH)) break;
    }
    if (i >= ST22_TEST_SHA_HIST_NUM) {
      test_sha_dump("st22p_rx_error_sha", sha);
      s->sha_fail_cnt++;
    }
    /* directly put */
    st22p_rx_put_frame((st22p_rx_handle)handle, frame);
    s->fb_rec++;
    if (!s->start_time) s->start_time = st_test_get_monotonic_time();
  }
  dbg("%s(%d), stop\n", __func__, s->idx);
}

struct st22p_rx_digest_test_para {
  int sessions;
  int fail_interval;
  int timeout_interval;
  int timeout_ms;
  int rand_ratio;
  bool check_fps;
  enum st_test_level level;
  bool user_timestamp;
  bool vsync;
  bool rtcp;
  bool tx_ext;
  bool rx_ext;
  bool interlace;
  uint32_t ssrc;
  bool block_get;
  bool codec_block_get;
  bool derive;
};

static void test_st22p_init_rx_digest_para(struct st22p_rx_digest_test_para* para) {
  memset(para, 0, sizeof(*para));

  para->sessions = 1;
  para->fail_interval = 0;
  para->timeout_interval = 0;
  para->timeout_ms = 0;
  para->rand_ratio = 0;
  para->check_fps = true;
  para->level = ST_TEST_LEVEL_MANDATORY;
  para->user_timestamp = false;
  para->vsync = true;
  para->rtcp = false;
  para->tx_ext = false;
  para->rx_ext = false;
  para->interlace = false;
  para->ssrc = 0;
  para->block_get = false;
  para->codec_block_get = false;
}

static void st22p_rx_digest_test(enum st_fps fps[], int width[], int height[],
                                 enum st_frame_fmt fmt[], enum st22_codec codec[],
                                 int compress_ratio[],
                                 struct st22p_rx_digest_test_para* para) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto st = ctx->handle;
  int ret;
  struct st22p_tx_ops ops_tx;
  struct st22p_rx_ops ops_rx;
  int sessions = para->sessions;

  st_test_jxs_fail_interval(ctx, para->fail_interval);
  st_test_jxs_timeout_interval(ctx, para->timeout_interval);
  st_test_jxs_timeout_ms(ctx, para->timeout_ms);
  st_test_jxs_rand_ratio(ctx, para->rand_ratio);
  st_test_jxs_use_block_get(ctx, para->codec_block_get);

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

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st22p_tx_handle> tx_handle;
  std::vector<st22p_rx_handle> rx_handle;
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
    test_ctx_tx[i]->fb_cnt = ST22_TEST_SHA_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->width = width[i];
    test_ctx_tx[i]->height = height[i];
    test_ctx_tx[i]->fmt = fmt[i];
    test_ctx_tx[i]->user_timestamp = para->user_timestamp;
    test_ctx_tx[i]->block_get = para->block_get;

    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22p_test";
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
    ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ST22P_TEST_UDP_PORT + i * 2;
    ops_tx.port.payload_type = ST22P_TEST_PAYLOAD_TYPE;
    ops_tx.port.ssrc = para->ssrc;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.interlaced = para->interlace;
    ops_tx.input_fmt = fmt[i];
    ops_tx.pack_type = ST22_PACK_CODESTREAM;
    ops_tx.codec = codec[i];
    ops_tx.device = ST_PLUGIN_DEVICE_TEST;
    ops_tx.quality = ST22_QUALITY_MODE_QUALITY;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    if (para->block_get)
      ops_tx.flags |= ST22P_TX_FLAG_BLOCK_GET;
    else
      ops_tx.notify_frame_available = test_st22p_tx_frame_available;
    ops_tx.notify_event = test_ctx_notify_event;
    ops_tx.notify_frame_done = test_st22p_tx_frame_done;
    if (para->user_timestamp) ops_tx.flags |= ST22P_TX_FLAG_USER_TIMESTAMP;
    if (para->vsync) ops_tx.flags |= ST22P_TX_FLAG_ENABLE_VSYNC;
    if (para->tx_ext) {
      ops_tx.flags |= ST22P_TX_FLAG_EXT_FRAME;
    }

    if (para->rtcp) {
      ops_tx.flags |= ST22P_TX_FLAG_ENABLE_RTCP;
      ops_tx.rtcp.buffer_size = 512;
    }

    if (para->derive) {
      ops_tx.codestream_size = ops_tx.width * ops_tx.height / compress_ratio[i];
      test_ctx_tx[i]->frame_size = ops_tx.codestream_size;
    } else {
      test_ctx_tx[i]->frame_size =
          st_frame_size(ops_tx.input_fmt, ops_tx.width, ops_tx.height, ops_tx.interlaced);
      ops_tx.codestream_size = test_ctx_tx[i]->frame_size / compress_ratio[i];
    }

    tx_handle[i] = st22p_tx_create(st, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    EXPECT_EQ(test_ctx_tx[i]->frame_size, st22p_tx_frame_size(tx_handle[i]));
    if (para->block_get) {
      ret = st22p_tx_set_block_timeout(tx_handle[i], NS_PER_S);
      EXPECT_EQ(ret, 0);
    }

    /* init ext frames, only for no convert */
    if (para->tx_ext) {
      uint8_t planes = st_frame_fmt_planes(fmt[i]);
      size_t frame_size = test_ctx_tx[i]->frame_size;

      test_ctx_tx[i]->p_ext_frames = (struct st_ext_frame*)malloc(
          sizeof(*test_ctx_tx[i]->p_ext_frames) * test_ctx_tx[i]->fb_cnt);
      size_t pg_sz = mtl_page_size(st);
      size_t fb_size = test_ctx_tx[i]->frame_size * test_ctx_tx[i]->fb_cnt;
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
              st_frame_least_linesize(fmt[i], width[i], plane);
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

    /* sha calculate */
    size_t frame_size = test_ctx_tx[i]->frame_size;
    uint8_t* fb;
    for (int frame = 0; frame < ST22_TEST_SHA_HIST_NUM; frame++) {
      if (para->tx_ext)
        fb = (uint8_t*)test_ctx_tx[i]->ext_fb + frame * frame_size;
      else
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
    test_ctx_rx[i]->fb_cnt = ST22_TEST_SHA_HIST_NUM;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->width = width[i];
    test_ctx_rx[i]->height = height[i];
    test_ctx_rx[i]->fmt = fmt[i];
    test_ctx_rx[i]->user_timestamp = para->user_timestamp;
    test_ctx_rx[i]->block_get = para->block_get;
    /* copy sha */
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           ST22_TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);

    if (para->rx_ext) {
      uint8_t planes = st_frame_fmt_planes(fmt[i]);
      test_ctx_rx[i]->p_ext_frames = (struct st_ext_frame*)malloc(
          sizeof(*test_ctx_rx[i]->p_ext_frames) * test_ctx_rx[i]->fb_cnt);
      size_t frame_size = st_frame_size(fmt[i], width[i], height[i], false);
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
              st_frame_least_linesize(fmt[i], width[i], plane);
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
    }

    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22p_test";
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
    ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ST22P_TEST_UDP_PORT + i * 2;
    ops_rx.port.payload_type = ST22P_TEST_PAYLOAD_TYPE;
    ops_rx.port.ssrc = para->ssrc;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.interlaced = para->interlace;
    ops_rx.output_fmt = fmt[i];
    ops_rx.pack_type = ST22_PACK_CODESTREAM;
    ops_rx.codec = codec[i];
    ops_rx.device = ST_PLUGIN_DEVICE_TEST;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    if (para->block_get)
      ops_rx.flags |= ST22P_RX_FLAG_BLOCK_GET;
    else
      ops_rx.notify_frame_available = test_st22p_rx_frame_available;
    ops_rx.notify_event = test_ctx_notify_event;
    if (para->vsync) ops_rx.flags |= ST22P_RX_FLAG_ENABLE_VSYNC;
    if (para->rx_ext) {
      ops_rx.flags |= ST22P_RX_FLAG_EXT_FRAME;
      ops_rx.query_ext_frame = test_st22p_rx_query_ext_frame;
    }

    if (para->rtcp) {
      ops_rx.flags |= ST22P_RX_FLAG_ENABLE_RTCP | ST22P_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 100;
      ops_rx.rtcp.seq_skip_window = 0;
      ops_rx.rtcp.burst_loss_max = 4;
      ops_rx.rtcp.sim_loss_rate = 0.0001;
    }

    if (para->derive)
      test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    else
      test_ctx_rx[i]->frame_size = st_frame_size(ops_rx.output_fmt, ops_rx.width,
                                                 ops_rx.height, ops_rx.interlaced);

    rx_handle[i] = st22p_rx_create(st, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    if (!para->derive) {
      EXPECT_EQ(test_ctx_rx[i]->frame_size, st22p_rx_frame_size(rx_handle[i]));
    }
    if (para->block_get) {
      ret = st22p_rx_set_block_timeout(rx_handle[i], NS_PER_S);
      EXPECT_EQ(ret, 0);
    }

    test_ctx_rx[i]->handle = rx_handle[i];

    rx_thread[i] = std::thread(test_st22p_rx_frame_thread, test_ctx_rx[i]);

    struct st_queue_meta meta;
    ret = st22p_rx_get_queue_meta(rx_handle[i], &meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(st);
  EXPECT_GE(ret, 0);
  sleep(10);
  ret = mtl_stop(st);
  EXPECT_GE(ret, 0);

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
    if (para->block_get) st22p_tx_wake_block(tx_handle[i]);
    test_ctx_tx[i]->cv.notify_all();
    tx_thread[i].join();
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
    EXPECT_NEAR(vsyncrate_rx[i], st_frame_rate(fps[i]), st_frame_rate(fps[i]) * 0.1);

    test_ctx_rx[i]->stop = true;
    if (para->block_get) st22p_rx_wake_block(rx_handle[i]);
    test_ctx_rx[i]->cv.notify_all();
    rx_thread[i].join();
  }

  for (int i = 0; i < sessions; i++) {
    ret = st22p_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    info("%s, session %d fb_send %d framerate %f:%f\n", __func__, i,
         test_ctx_tx[i]->fb_send, framerate_tx[i], expect_framerate_tx[i]);
    EXPECT_GE(test_ctx_rx[i]->fb_send, 0);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_frame_cnt, 0);
    if (para->tx_ext) {
      mtl_dma_unmap(st, test_ctx_tx[i]->ext_fb, test_ctx_tx[i]->ext_fb_iova,
                    test_ctx_tx[i]->ext_fb_iova_map_sz);
      st_test_free(test_ctx_tx[i]->ext_fb_malloc);
      st_test_free(test_ctx_tx[i]->p_ext_frames);
    }
    delete test_ctx_tx[i];
  }
  for (int i = 0; i < sessions; i++) {
    ret = st22p_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    info("%s, session %d fb_rec %d framerate %f:%f\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate_rx[i], expect_framerate_rx[i]);
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_frame_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
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
    delete test_ctx_rx[i];
  }
}

TEST(St22p, digest_st22_1080p_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.level = ST_TEST_LEVEL_ALL;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

TEST(St22p, digest_st22_1080i) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.level = ST_TEST_LEVEL_MANDATORY;
  para.interlace = true;
  para.block_get = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

TEST(St22p, digest_st22_4k_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 2};
  int height[1] = {1080 * 2};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {20};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.level = ST_TEST_LEVEL_ALL;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

TEST(St22p, digest_st22_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE,
                              ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[2] = {ST22_CODEC_JPEGXS, ST22_CODEC_JPEGXS};
  int compress_ratio[2] = {10, 16};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.user_timestamp = true;
  para.ssrc = 778899;
  para.codec_block_get = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

TEST(St22p, digest_st22_1080p_fail_interval) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.fail_interval = 3;
  para.block_get = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

TEST(St22p, digest_st22_1080p_timeout_interval) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.timeout_interval = 3;
  para.timeout_ms = 20;
  para.codec_block_get = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

TEST(St22p, digest_st22_1080p_rand_size) {
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR8};
  enum st22_codec codec[1] = {ST22_CODEC_H264_CBR};
  int compress_ratio[1] = {5};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.rand_ratio = 30;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

TEST(St22p, digest_st22_s2_rtcp) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE,
                              ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[2] = {ST22_CODEC_JPEGXS, ST22_CODEC_JPEGXS};
  int compress_ratio[2] = {10, 16};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.check_fps = false;
  para.rtcp = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

TEST(St22p, digest_st22_s2_ext) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt fmt[2] = {ST_FRAME_FMT_YUV422PLANAR10LE,
                              ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[2] = {ST22_CODEC_JPEGXS, ST22_CODEC_JPEGXS};
  int compress_ratio[2] = {10, 16};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.tx_ext = true;
  para.rx_ext = true;
  para.codec_block_get = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

TEST(St22p, digest_derive_s2) {
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  enum st_frame_fmt fmt[2] = {ST_FRAME_FMT_JPEGXS_CODESTREAM,
                              ST_FRAME_FMT_H264_CBR_CODESTREAM};
  enum st22_codec codec[2] = {ST22_CODEC_JPEGXS, ST22_CODEC_H264_CBR};
  int compress_ratio[2] = {5, 8};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.sessions = 2;
  para.derive = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}
