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

/**
 * @file st_pipeline_api.h
 *
 * Interfaces to Intel(R) Media Streaming Library Pipeline APIs
 *
 * This header define the public interfaces of Intel(R) Media Streaming Library Pipeline
 * APIs
 *
 */

#include <st_dpdk_api.h>

#ifndef _ST_PIPELINE_API_HEAD_H_
#define _ST_PIPELINE_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** Handle to tx st2110-22 pipeline session of lib */
typedef struct st22p_tx_ctx* st22p_tx_handle;
/** Handle to rx st2110-22 pipeline session of lib */
typedef struct st22p_rx_ctx* st22p_rx_handle;

/** Handle to st2110-22 encode device of lib */
typedef struct st22_encode_dev_impl* st22_encoder_dev_handle;
/** Handle to st2110-22 decode device of lib */
typedef struct st22_decode_dev_impl* st22_decoder_dev_handle;

/** Handle to the encode session private data of plugin */
typedef void* st22_encode_priv;

/** Handle to the st2110-22 pipeline encode session of lib */
typedef struct st22_encode_session_impl* st22p_encode_session;

/** Handle to the decode session private data of plugin */
typedef void* st22_decode_priv;

/** Handle to the st2110-22 pipeline decode session of lib */
typedef struct st22_decode_session_impl* st22p_decode_session;

/** Handle to the private data of plugin */
typedef void* st_plugin_priv;

/** Version type of st plugin */
enum st_plugin_version {
  /** auto */
  ST_PLUGIN_VERSION_UNKNOWN = 0,
  /** V1 */
  ST_PLUGIN_VERSION_V1,
  /** max value of this enum */
  ST_PLUGIN_VERSION_MAX,
};

/** Macro to compute a plugin magic */
#define ST_PLUGIN_MAGIC(a, b, c, d) ((a) << 24 | (b) << 16 | (c) << 16 | (d))

/** Macro to plugin magic of V1 */
#define ST_PLUGIN_VERSION_V1_MAGIC ST_PLUGIN_MAGIC('p', 'l', 'v', '1')

/** The structure info for plugin meta. */
struct st_plugin_meta {
  /** plugin version */
  enum st_plugin_version version;
  /** plugin magic */
  uint32_t magic;
};

/** Get meta function porotype of plugin */
typedef int (*st_plugin_get_meta_fn)(struct st_plugin_meta* meta);
/** Get meta function name of plugin */
#define ST_PLUGIN_GET_META_API "st_plugin_get_meta"
/** Create function porotype of plugin */
typedef st_plugin_priv (*st_plugin_create_fn)(st_handle st);
/** Create function name of plugin */
#define ST_PLUGIN_CREATE_API "st_plugin_create"
/** Free function porotype of plugin */
typedef int (*st_plugin_free_fn)(st_plugin_priv handle);
/** Free function name of plugin */
#define ST_PLUGIN_FREE_API "st_plugin_free"

/** Frame format */
enum st_frame_fmt {
  /** YUV 422 planar 10bit little endian */
  ST_FRAME_FMT_YUV422PLANAR10LE = 0,
  /** YUV 422 packed, 3 samples on a 32-bit word, 10 bits per sample */
  ST_FRAME_FMT_V210,
  /** YUV 422 planar 8bit */
  ST_FRAME_FMT_YUV422PLANAR8,
  /** YUV 422 packed 8bit */
  ST_FRAME_FMT_YUV422PACKED8,
  /** RFC4175 in ST2110, two YUV 422 10 bit pixel gruops on 5 bytes, big endian */
  ST_FRAME_FMT_YUV422RFC4175PG2BE10,
  /** one ARGB pixel per 32 bit word, 8 bits per sample */
  ST_FRAME_FMT_ARGB = 8,
  /** one BGRA pixel per 32 bit word, 8 bits per sample */
  ST_FRAME_FMT_BGRA,
  /** one RGB pixel per 24 bit word, 8 bits per sample */
  ST_FRAME_FMT_RGB8,
  /** ST22 jpegxs codestream */
  ST_FRAME_FMT_JPEGXS_CODESTREAM = 24,
  /** max value of this enum */
  ST_FRAME_FMT_MAX,
};

/** ST format cap of ST_FRAME_FMT_YUV422PLANAR10LE, used in the jpegxs_plugin caps */
#define ST_FMT_CAP_YUV422PLANAR10LE (ST_BIT64(ST_FRAME_FMT_YUV422PLANAR10LE))
/** ST format cap of ST_FRAME_FMT_V210, used in the jpegxs_plugin caps */
#define ST_FMT_CAP_V210 (ST_BIT64(ST_FRAME_FMT_V210))
/** ST format cap of ST_FRAME_FMT_YUV422PLANAR8, used in the jpegxs_plugin caps */
#define ST_FMT_CAP_YUV422PLANAR8 (ST_BIT64(ST_FRAME_FMT_YUV422PLANAR8))
/** ST format cap of ST_FRAME_FMT_YUV422PACKED8, used in the jpegxs_plugin caps */
#define ST_FMT_CAP_YUV422PACKED8 (ST_BIT64(ST_FRAME_FMT_YUV422PACKED8))
/** ST format cap of ST_FRAME_FMT_YUV422RFC4175PG2BE10, used in the jpegxs_plugin caps */
#define ST_FMT_CAP_YUV422RFC4175PG2BE10 (ST_BIT64(ST_FRAME_FMT_YUV422RFC4175PG2BE10))

/** ST format cap of ST_FRAME_FMT_ARGB, used in the jpegxs_plugin caps */
#define ST_FMT_CAP_ARGB (ST_BIT64(ST_FRAME_FMT_ARGB))
/** ST format cap of ST_FRAME_FMT_ARGB, used in the jpegxs_plugin caps */
#define ST_FMT_CAP_BGRA (ST_BIT64(ST_FRAME_FMT_BGRA))
/** ST format cap of ST_FRAME_FMT_RGB8, used in the jpegxs_plugin caps */
#define ST_FMT_CAP_RGB8 (ST_BIT64(ST_FRAME_FMT_RGB8))

/** ST format cap of ST_FRAME_FMT_JPEGXS_CODESTREAM, used in the jpegxs_plugin caps */
#define ST_FMT_CAP_JPEGXS_CODESTREAM (ST_BIT64(ST_FRAME_FMT_JPEGXS_CODESTREAM))

/** The structure info for frame meta. */
struct st_frame_meta {
  /** frame pointer address, allocated from lib */
  void* addr;
  /** frame format */
  enum st_frame_fmt fmt;
  /** frame buffer size */
  size_t buffer_size;
  /**
   * frame valid data size, may <= buffer_size for one encoded frame.
   * encode dev put the real codestream size here.
   * Same for decode.
   */
  size_t data_size;
  /** frame resolution width */
  uint32_t width;
  /** frame resolution height */
  uint32_t height;
  /** index in frame buffers */
  uint16_t idx;
  /** frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** frame timestamp value */
  uint64_t timestamp;

  /** priv pointer for lib, do not touch this */
  void* priv;
};

/** Device type of st plugin */
enum st_plugin_device {
  /** auto */
  ST_PLUGIN_DEVICE_AUTO = 0,
  /** CPU */
  ST_PLUGIN_DEVICE_CPU,
  /** GPU */
  ST_PLUGIN_DEVICE_GPU,
  /** FPGA */
  ST_PLUGIN_DEVICE_FPGA,
  /** For test only, don't use */
  ST_PLUGIN_DEVICE_TEST,
  /** max value of this enum */
  ST_PLUGIN_DEVICE_MAX,
};

/** Codec type of st22 */
enum st22_codec {
  /** jpegxs codec */
  ST22_CODEC_JPEGXS = 0,
  /** max value of this enum */
  ST22_CODEC_MAX,
};

/** Qulity mode type of st22, speed or quality */
enum st22_quality_mode {
  /** speed mode */
  ST22_QUALITY_MODE_SPEED = 0,
  /** quality mode */
  ST22_QUALITY_MODE_QUALITY,
  /** max value of this enum */
  ST22_QUALITY_MODE_MAX,
};

/** The structure info for st plugin encode session create request. */
struct st22_encoder_create_req {
  /** codestream size required */
  size_t codestream_size;
  /** Session resolution width, set by lib */
  uint32_t width;
  /** Session resolution height, set by lib */
  uint32_t height;
  /** Session resolution fps, set by lib */
  enum st_fps fps;
  /** Session input frame format, set by lib */
  enum st_frame_fmt input_fmt;
  /** Session output frame format, set by lib */
  enum st_frame_fmt output_fmt;
  /** speed or quality mode, set by lib */
  enum st22_quality_mode quality;
  /** frame buffer count, set by lib */
  uint16_t framebuff_cnt;
  /** thread count, set by lib */
  uint32_t codec_thread_cnt;

  /** max size for frame(encoded code stream), set by plugin */
  size_t max_codestream_size;
};

/** The structure info for st22 encoder dev. */
struct st22_encoder_dev {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** codec this plugin supported */
  enum st22_codec codec;
  /** device, cpu/gpu/fpga/others */
  enum st_plugin_device target_device;

  /** supported input format for encode, ST_FMT_CAP_* */
  uint64_t input_fmt_caps;
  /** supported output format for encode, ST_FMT_CAP_* */
  uint64_t output_fmt_caps;
  /** create session funtion */
  st22_encode_priv (*create_session)(void* priv, st22p_encode_session session_p,
                                     struct st22_encoder_create_req* req);
  /** Callback when frame available in the lib. */
  int (*notify_frame_available)(st22_encode_priv encode_priv);
  /** free session funtion */
  int (*free_session)(void* priv, st22_encode_priv encode_priv);
};

/** The structure info for st22 encode frame meta. */
struct st22_encode_frame_meta {
  /** Encode source frame */
  struct st_frame_meta* src;
  /** Encode dst frame */
  struct st_frame_meta* dst;
  /** priv pointer for lib, do not touch this */
  void* priv;
};

/** The structure info for st plugin decode session create request. */
struct st22_decoder_create_req {
  /** Session resolution width, set by lib */
  uint32_t width;
  /** Session resolution height, set by lib */
  uint32_t height;
  /** Session resolution fps, set by lib */
  enum st_fps fps;
  /** Session input frame format, set by lib */
  enum st_frame_fmt input_fmt;
  /** Session output frame format, set by lib */
  enum st_frame_fmt output_fmt;
  /** frame buffer count, set by lib */
  uint16_t framebuff_cnt;
  /** thread count, set by lib */
  uint32_t codec_thread_cnt;
};

/** The structure info for st22 decoder dev. */
struct st22_decoder_dev {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** codec this plugin supported */
  enum st22_codec codec;
  /** device, cpu/gpu/fpga/others */
  enum st_plugin_device target_device;

  /** supported input format for decode, ST_FMT_CAP_* */
  uint64_t input_fmt_caps;
  /** supported output format for decode, ST_FMT_CAP_* */
  uint64_t output_fmt_caps;
  /** create session funtion */
  st22_decode_priv (*create_session)(void* priv, st22p_decode_session session_p,
                                     struct st22_decoder_create_req* req);
  /** Callback when frame available in the lib. */
  int (*notify_frame_available)(st22_decode_priv decode_priv);
  /** free session funtion */
  int (*free_session)(void* priv, st22_decode_priv decode_priv);
};

/** The structure info for st22 decode frame meta. */
struct st22_decode_frame_meta {
  /** Encode source frame */
  struct st_frame_meta* src;
  /** Encode dst frame */
  struct st_frame_meta* dst;
  /** priv pointer for lib, do not touch this */
  void* priv;
};

/** The structure info for st tx port, used in creating session. */
struct st_tx_port {
  /** destination IP address */
  uint8_t dip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of st_init */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
};

/** The structure info for st rx port, used in creating session. */
struct st_rx_port {
  /** source IP address of sender */
  uint8_t sip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of st_init */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
};

/** The structure describing how to create a tx st2110-22 pipeline session. */
struct st22p_tx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** tx port info */
  struct st_tx_port port;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session input frame format */
  enum st_frame_fmt input_fmt;
  /** packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;
  /** codec for this pipeline */
  enum st22_codec codec;
  /** encode plugin device, auto or special */
  enum st_plugin_device device;
  /** speed or quality mode */
  enum st22_quality_mode quality;
  /** thread count for codec, leave to zero if not know */
  uint32_t codec_thread_cnt;
  /** codestream size, calculate as compress ratio */
  size_t codestream_size;
  /**
   * the frame buffer count requested for one st22 pipeline tx session,
   * should be in range [2, ST22_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;
  /**
   * Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
};

/** The structure describing how to create a rx st2110-22 pipeline session. */
struct st22p_rx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** tx port info */
  struct st_rx_port port;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session output frame format */
  enum st_frame_fmt output_fmt;
  /** packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;
  /** codec for this pipeline */
  enum st22_codec codec;
  /** encode plugin device, auto or special */
  enum st_plugin_device device;
  /** thread count for codec, leave to zero if not know */
  uint32_t codec_thread_cnt;
  /** max codestream size, lib will use output frame size if not set */
  size_t max_codestream_size;
  /**
   * the frame buffer count requested for one st22 pipeline rx session,
   * should be in range [2, ST22_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;
  /**
   * Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
};

/**
 * Register one st22 encoder.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param dev
 *   The pointer to the structure describing a st plugin encode.
 * @return
 *   - NULL: fail.
 *   - Others: the handle to the encode dev context.
 */
st22_encoder_dev_handle st22_encoder_register(st_handle st, struct st22_encoder_dev* dev);

/**
 * Unregister one st22 encoder.
 *
 * @param handle
 *   The handle to the the encode dev context.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st22_encoder_unregister(st22_encoder_dev_handle handle);

/**
 * Get one encode frame from the tx st2110-22 pipeline session.
 * Call st22_encoder_put_frame to return the frame to session.
 *
 * @param session
 *   The handle to the tx st2110-22 pipeline session.
 * @return
 *   - NULL if no avaiable frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st22_encode_frame_meta* st22_encoder_get_frame(st22p_encode_session session);

/**
 * Put back the frame which get by st22_encoder_get_frame to the tx
 * st2110-22 pipeline session.
 *
 * @param session
 *   The handle to the tx st2110-22 pipeline session.
 * @param frame
 *   the frame pointer by st22_encoder_get_frame.
 * @param result
 *   the encode result for current frame, < 0 means fail.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22_encoder_put_frame(st22p_encode_session session,
                           struct st22_encode_frame_meta* frame, int result);

/**
 * Register one st22 decoder.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param dev
 *   The pointer to the structure describing a st plugin encode.
 * @return
 *   - NULL: fail.
 *   - Others: the handle to the encode dev
 */
st22_decoder_dev_handle st22_decoder_register(st_handle st, struct st22_decoder_dev* dev);

/**
 * Unregister one st22 decoder.
 *
 * @param handle
 *   The handle to the the decode dev context.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st22_decoder_unregister(st22_decoder_dev_handle handle);

/**
 * Get one decode frame from the rx st2110-22 pipeline session.
 * Call st22_decoder_put_frame to return the frame to session.
 *
 * @param session
 *   The handle to the rx st2110-22 pipeline session.
 * @return
 *   - NULL if no avaiable frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st22_decode_frame_meta* st22_decoder_get_frame(st22p_decode_session session);

/**
 * Put back the frame which get by st22_decoder_get_frame to the rx
 * st2110-22 pipeline session.
 *
 * @param session
 *   The handle to the rx st2110-22 pipeline session.
 * @param frame
 *   the frame pointer by st22_encoder_get_frame.
 * @param result
 *   the decode result for current frame, < 0 means fail.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22_decoder_put_frame(st22p_decode_session session,
                           struct st22_decode_frame_meta* frame, int result);

/**
 * Register one st plugin so.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param path
 *   The path to the plugin so.
 *   Ex: /usr/local/lib/x86_64-linux-gnu/libst_plugin_sample.so
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st_plugin_register(st_handle st, const char* path);

/**
 * Unregister one st plugin so.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param path
 *   The path to the plugin so.
 *   Ex: /usr/local/lib/x86_64-linux-gnu/libst_plugin_sample.so
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st_plugin_unregister(st_handle st, const char* path);

/**
 * Get the number of registered plugins lib.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - number.
 */
int st_get_plugins_nb(st_handle st);

/**
 * Create one tx st2110-22 pipeline session.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx
 * st2110-22 pipeline session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-22 pipeline session.
 */
st22p_tx_handle st22p_tx_create(st_handle st, struct st22p_tx_ops* ops);

/**
 * Free the tx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @return
 *   - 0: Success, tx st2110-22 pipeline session freed.
 *   - <0: Error code of the tx st2110-22 pipeline session free.
 */
int st22p_tx_free(st22p_tx_handle handle);

/**
 * Get one tx frame from the tx st2110-22 pipeline session.
 * Call st22p_tx_put_frame to return the frame to session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @return
 *   - NULL if no avaiable frame in the session.
 *   - Otherwise, the frame meta pointer.
 */
struct st_frame_meta* st22p_tx_get_frame(st22p_tx_handle handle);

/**
 * Put back the frame which get by st22p_tx_get_frame to the tx
 * st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @param frame
 *   the frame pointer by st22p_tx_get_frame.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22p_tx_put_frame(st22p_tx_handle handle, struct st_frame_meta* frame);

/**
 * Get the framebuffer pointer from the tx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st22p_tx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st22p_tx_get_fb_addr(st22p_tx_handle handle, uint16_t idx);

/**
 * Get the framebuffer size from the tx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @return
 *   - size
 */
size_t st22p_tx_frame_size(st22p_tx_handle handle);

/**
 * Create one rx st2110-22 pipeline session.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 * st2110-22 pipeline session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-22 pipeline session.
 */
st22p_rx_handle st22p_rx_create(st_handle st, struct st22p_rx_ops* ops);

/**
 * Free the rx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @return
 *   - 0: Success, rx st2110-22 pipeline session freed.
 *   - <0: Error code of the rx st2110-22 pipeline session free.
 */
int st22p_rx_free(st22p_rx_handle handle);

/**
 * Get one rx frame from the rx st2110-22 pipeline session.
 * Call st22p_rx_put_frame to return the frame to session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @return
 *   - NULL if no avaiable frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st_frame_meta* st22p_rx_get_frame(st22p_rx_handle handle);

/**
 * Put back the frame which get by st22p_rx_get_frame to the rx
 * st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @param frame
 *   the frame pointer by st22p_rx_get_frame.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22p_rx_put_frame(st22p_rx_handle handle, struct st_frame_meta* frame);

/**
 * Get the framebuffer pointer from the rx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st22p_rx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st22p_rx_get_fb_addr(st22p_rx_handle handle, uint16_t idx);

/**
 * Get the framebuffer size from the rx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @return
 *   - size
 */
size_t st22p_rx_frame_size(st22p_rx_handle handle);

/**
 * Dump st2110-22 pipeline packets to pcapng file.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @param max_dump_packets
 *   The max number of packets to be dumpped.
 * @param sync
 *   synchronous or asynchronous, true means this func will return after dump
 * progress is finished.
 * @param meta
 *   The meta data returned, only for synchronous, leave to NULL if not need the meta.
 * @return
 *   - 0: Success, rx st2110-22 pipeline session pcapng dump succ.
 *   - <0: Error code of the rx st2110-22 pipeline session pcapng dump.
 */
int st22p_rx_pcapng_dump(st22p_rx_handle handle, uint32_t max_dump_packets, bool sync,
                         struct st_pcap_dump_meta* meta);

/**
 * Calculate the frame size per the format, w and h
 *
 * @param fmt
 *   format.
 * @param width
 *   width.
 * @param height
 *   height.
 * @return
 *   > 0 if successful.
 *   0: Fail.
 */
size_t st_frame_size(enum st_frame_fmt fmt, uint32_t width, uint32_t height);

/**
 * Get the name of st_frame_fmt
 *
 * @param fmt
 *   format.
 * @return
 *   The pointer to name.
 *   NULL: Fail.
 */
const char* st_frame_fmt_name(enum st_frame_fmt fmt);

/**
 * Draw a logo on the frame, only ST_FRAME_FMT_YUV422RFC4175PG2BE10 now
 *
 * @param frame
 *   the frame meta pointer of the source.
 * @param logo
 *   the frame meta pointer of the logo.
 * @param x
 *   the logo position x.
 * @param y
 *   the logo position y.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st_draw_logo(struct st_frame_meta* frame, struct st_frame_meta* logo, uint32_t x,
                 uint32_t y);

#if defined(__cplusplus)
}
#endif

#endif
