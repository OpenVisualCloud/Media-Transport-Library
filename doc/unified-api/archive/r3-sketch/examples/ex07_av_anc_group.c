/* 10 §9 — A/V/ANC from one file (P3), the maintainer's question: three sessions on one
   lazily anchored timeline, started together, exact RTP for every essence. */
#include "ex_common.h"

enum { STREAM_VIDEO = 0, STREAM_AUDIO = 1 };
/* the demuxer: 0 = packet, < 0 = end; video pts in 1001/60000 (= frame index), audio pts
   in 1/48000 (= first sample index) */
int demux_next(int* stream, int64_t* pts, const void** data, size_t* size,
               uint32_t* samples);
int captions_for(int64_t pts, const struct mtl_anc_packet** pkts, uint32_t* n,
                 const void** udw, size_t* udw_len);
void decode_into(const struct mtl_buffer_view* v, const void* data, size_t size);

static void playout_timing(struct mtl_session_config* sc, mtl_timeline_h tl) {
  sc->timing.timeline = tl;
  sc->timing.media_mode = MTL_MEDIA_INDEX;
  sc->timing.source_kind = MTL_SOURCE_PLAYBACK;
}

static int demux_loop(mtl_session_h video, mtl_session_h audio, mtl_session_h anc) {
  int stream, ret = 0;
  int64_t pts;
  const void* data;
  size_t size;
  uint32_t samples;
  while (ret >= 0 && g_running &&
         demux_next(&stream, &pts, &data, &size, &samples) == 0) {
    struct mtl_tx_submission sub;
    mtl_tx_submission_init(&sub);
    sub.media_index = pts;
    /* packets straddling submissions use the carry buffer */
    if (stream == STREAM_AUDIO) {
      sub.sample_count = samples;
      ret = mtl_tx_write(audio, data, size, &sub, MTL_MS(20));
      continue;
    }
    const struct mtl_anc_packet* pkts;
    const void* udw;
    size_t udw_len;
    uint32_t n;
    /* ANC k before or with video k */
    if (captions_for(pts, &pkts, &n, &udw, &udw_len) == 0) {
      sub.anc = pkts;
      sub.anc_count = n;
      ret = mtl_tx_write(anc, udw, udw_len, &sub, MTL_MS(20));
      if (ret < 0) break;
      sub.anc = NULL;
      sub.anc_count = 0;
    } /* frames without captions get the empty ANC packet (MTL_UNDERRUN_EMPTY_ANC) */
    mtl_lease_h lease;
    struct mtl_buffer_view v;
    mtl_buffer_view_init(&v);
    ret = mtl_tx_acquire(video, &lease, &v, NULL, MTL_MS(40));
    if (ret < 0) break;
    decode_into(&v, data, size);
    ret = mtl_tx_submit(video, lease, &sub);
    if (ret < 0) mtl_tx_release(video, lease);
  }
  return ret < 0 ? ex_fail("playout", ret) : 0;
}

int av_anc_playout(mtl_instance_h mt, const struct mtl_video_config* vc,
                   const struct mtl_audio_config* ac, const struct mtl_anc_config* nc) {
  struct mtl_timeline_config tc;
  /* anchor_mode 0 = MTL_ANCHOR_AT_START: T0 at group start */
  mtl_timeline_config_init(&tc);
  mtl_timeline_h tl;
  int ret = mtl_timeline_create(mt, &tc, &tl);
  if (ret < 0) return ex_fail("timeline", ret);

  struct mtl_session_config scv, sca, scn;
  mtl_session_config_init(&scv);
  scv.direction = MTL_DIR_TX;
  mtl_session_config_init(&sca);
  sca.direction = MTL_DIR_TX;
  mtl_session_config_init(&scn);
  scn.direction = MTL_DIR_TX;
  playout_timing(&scv, tl);
  playout_timing(&sca, tl);
  playout_timing(&scn, tl);
  if (mtl_flow_parse("239.168.85.20:20000", &scv.flows[0]) < 0 ||
      mtl_flow_parse("239.168.85.20:30000", &sca.flows[0]) < 0 ||
      mtl_flow_parse("239.168.85.20:40000", &scn.flows[0]) < 0)
    return -MTL_EINVAL;

  mtl_session_h video = MTL_SESSION_NULL, audio = MTL_SESSION_NULL,
                anc = MTL_SESSION_NULL;
  mtl_group_h g = MTL_GROUP_NULL;
  ret = mtl_video_session_create(mt, &scv, vc, &video);               /* 1080p59.94 */
  if (ret >= 0) ret = mtl_audio_session_create(mt, &sca, ac, &audio); /* 48 kHz, S = 48 */
  /* fps + total_lines 1125 */
  if (ret >= 0) ret = mtl_anc_session_create(mt, &scn, nc, &anc);
  if (ret >= 0) ret = mtl_group_create(mt, tl, NULL, &g);
  if (ret >= 0) ret = mtl_group_add(g, video);
  if (ret >= 0) ret = mtl_group_add(g, audio);
  if (ret >= 0) ret = mtl_group_add(g, anc);

  struct mtl_start_params sp;
  mtl_start_params_init(&sp);
  /* T0 = ceil((now + lead + preroll - k*period) / G) * G */
  sp.mode = MTL_START_AT_MEDIA_INDEX;
  sp.media_index = 0;
  struct mtl_time t0;
  /* all members ARMED together, or none */
  if (ret >= 0) ret = mtl_group_start(g, &sp, &t0);
  if (ret >= 0)
    ret = demux_loop(video, audio, anc);
  else
    ex_fail("setup", ret);

  if (!mtl_group_is_null(g)) {
    mtl_group_stop(g, MTL_STOP_DRAIN, MTL_SEC(2));
    mtl_group_destroy(g);
  }
  if (!mtl_session_is_null(anc)) mtl_session_destroy(anc, 0);
  if (!mtl_session_is_null(audio)) mtl_session_destroy(audio, 0);
  if (!mtl_session_is_null(video)) mtl_session_destroy(video, 0);
  mtl_timeline_destroy(tl);
  return ret;
}
