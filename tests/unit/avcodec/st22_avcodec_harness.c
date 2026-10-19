/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "avcodec/st22_avcodec_harness.h"

#include "../../../plugins/st22_avcodec/st22_avcodec_plugin.c"

#define UT_PLANE_SIZE 4

static uint8_t ut_src_planes[3][UT_PLANE_SIZE];
static uint8_t ut_codec_planes[3][UT_PLANE_SIZE];
static AVFrame ut_codec_frame = {
    .data = {ut_codec_planes[0], ut_codec_planes[1], ut_codec_planes[2]}};
static AVPacket ut_codec_pkt;
static uint8_t* ut_pkt_data;
static int ut_pkt_size;
static int ut_pkt_refs;

const AVCodec* avcodec_find_encoder(enum AVCodecID id) {
  return NULL;
}

const AVCodec* avcodec_find_decoder(enum AVCodecID id) {
  return NULL;
}

AVCodecContext* avcodec_alloc_context3(const AVCodec* codec) {
  return NULL;
}

void avcodec_free_context(AVCodecContext** avctx) {
  *avctx = NULL;
}

int avcodec_open2(AVCodecContext* avctx, const AVCodec* codec, AVDictionary** options) {
  return AVERROR(ENOSYS);
}

int avcodec_send_frame(AVCodecContext* avctx, const AVFrame* frame) {
  return 0;
}

int avcodec_receive_packet(AVCodecContext* avctx, AVPacket* avpkt) {
  avpkt->data = ut_pkt_data;
  avpkt->size = ut_pkt_size;
  ut_pkt_refs++;
  return 0;
}

int avcodec_send_packet(AVCodecContext* avctx, const AVPacket* avpkt) {
  return AVERROR(ENOSYS);
}

int avcodec_receive_frame(AVCodecContext* avctx, AVFrame* frame) {
  return AVERROR(ENOSYS);
}

AVFrame* av_frame_alloc(void) {
  return NULL;
}

void av_frame_free(AVFrame** frame) {
  *frame = NULL;
}

int av_frame_get_buffer(AVFrame* frame, int align) {
  return AVERROR(ENOSYS);
}

void av_frame_unref(AVFrame* frame) {
}

AVPacket* av_packet_alloc(void) {
  return NULL;
}

void av_packet_free(AVPacket** pkt) {
  *pkt = NULL;
}

void av_packet_unref(AVPacket* pkt) {
  if (pkt->data) ut_pkt_refs--;
  pkt->data = NULL;
  pkt->size = 0;
}

AVCodecParserContext* av_parser_init(int codec_id) {
  return NULL;
}

void av_parser_close(AVCodecParserContext* s) {
}

int av_opt_set(void* obj, const char* name, const char* val, int search_flags) {
  return 0;
}

void ut_avcodec_reset(void) {
  memset(&ut_codec_pkt, 0, sizeof(ut_codec_pkt));
  ut_pkt_data = NULL;
  ut_pkt_size = 0;
  ut_pkt_refs = 0;
}

void ut_avcodec_set_packet(uint8_t* data, int size) {
  ut_pkt_data = data;
  ut_pkt_size = size;
}

int ut_avcodec_packet_refs(void) {
  return ut_pkt_refs;
}

int ut_avcodec_encode_frame(struct st_frame* dst) {
  struct st_frame src = {
      .addr = {ut_src_planes[0], ut_src_planes[1], ut_src_planes[2]},
      .linesize = {2, 1, 1},
      .fmt = ST_FRAME_FMT_YUV422PLANAR8,
      .width = 2,
      .height = 2,
  };
  struct st22_avcodec_encoder_session session = {
      .codec_ctx = NULL,
      .codec_frame = &ut_codec_frame,
      .codec_pkt = &ut_codec_pkt,
  };
  struct st22_encode_frame_meta frame = {.src = &src, .dst = dst};

  return avcodec_encode_frame(&session, &frame);
}
