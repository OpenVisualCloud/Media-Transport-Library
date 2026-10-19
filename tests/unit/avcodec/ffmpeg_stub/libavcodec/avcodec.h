/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/* Only the FFmpeg surface st22_avcodec_plugin.c uses; enum values are not FFmpeg's. */

#ifndef TESTS_UNIT_AVCODEC_FFMPEG_STUB_LIBAVCODEC_AVCODEC_H
#define TESTS_UNIT_AVCODEC_FFMPEG_STUB_LIBAVCODEC_AVCODEC_H

#include <inttypes.h>

#define AVERROR(e) (-(e))
#define AVERROR_EOF (-0x20464f45)
#define av_err2str(errnum) "av error"

typedef struct AVRational {
  int num;
  int den;
} AVRational;

typedef struct AVDictionary AVDictionary;
typedef struct AVCodecParserContext AVCodecParserContext;

enum AVCodecID { AV_CODEC_ID_H264, AV_CODEC_ID_H265 };
enum AVPixelFormat { AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P };
enum AVPictureType { AV_PICTURE_TYPE_I };

typedef struct AVCodec {
  enum AVCodecID id;
} AVCodec;

typedef struct AVCodecContext {
  void* priv_data;
  int64_t bit_rate;
  int64_t rc_max_rate;
  int rc_buffer_size;
  int width;
  int height;
  AVRational time_base;
  AVRational framerate;
  enum AVPixelFormat pix_fmt;
} AVCodecContext;

typedef struct AVFrame {
  uint8_t* data[8];
  int width;
  int height;
  int format;
  enum AVPictureType pict_type;
  int64_t pts;
} AVFrame;

typedef struct AVPacket {
  int64_t pts;
  uint8_t* data;
  int size;
} AVPacket;

const AVCodec* avcodec_find_encoder(enum AVCodecID id);
const AVCodec* avcodec_find_decoder(enum AVCodecID id);
AVCodecContext* avcodec_alloc_context3(const AVCodec* codec);
void avcodec_free_context(AVCodecContext** avctx);
int avcodec_open2(AVCodecContext* avctx, const AVCodec* codec, AVDictionary** options);
int avcodec_send_frame(AVCodecContext* avctx, const AVFrame* frame);
int avcodec_receive_packet(AVCodecContext* avctx, AVPacket* avpkt);
int avcodec_send_packet(AVCodecContext* avctx, const AVPacket* avpkt);
int avcodec_receive_frame(AVCodecContext* avctx, AVFrame* frame);
AVFrame* av_frame_alloc(void);
void av_frame_free(AVFrame** frame);
int av_frame_get_buffer(AVFrame* frame, int align);
void av_frame_unref(AVFrame* frame);
AVPacket* av_packet_alloc(void);
void av_packet_free(AVPacket** pkt);
void av_packet_unref(AVPacket* pkt);
AVCodecParserContext* av_parser_init(int codec_id);
void av_parser_close(AVCodecParserContext* s);

#endif
