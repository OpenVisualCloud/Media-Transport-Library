/* ex17 — audio received as host PCM (an AES67 or ST 2110-30 bridge, a framework audio
   source). Plane 0 holds the samples as on the wire: one row per sample time, the
   channels interleaved, each sample big-endian (L24: three bytes); MTL never swaps them,
   so the reader does. media_index is the unit's first sample, counted on the epoch at the
   sample rate, so a jump in it is lost time: missed_before counts the units a full pool
   could not take, and the packets lost inside a unit read as zero, silence, with status
   MTL_RX_INCOMPLETE. s: a started RX session, MTL_AUDIO with MTL_PCM24. Needs: MS4 (audio
   MS4a1; media_index valid from MS3). */
#include "ex_common.h"

/* The framework's push: frames of interleaved int32 samples; first = the epoch index of
   the first sample (-1 = unknown); discont = 1 after lost time (GStreamer DISCONT). */
int push_pcm(const int32_t* pcm, uint32_t frames, int64_t first, int discont);

/* pcm: room for cap samples (frames x channels). */
int rx_audio(mtl_session_h s, int32_t* pcm, uint32_t cap) {
  struct mtl_unit u;
  int64_t next = -1; /* the sample expected next */
  int ret = 0;
  MTL_INIT(&u);
  while (ret >= 0 && g_running) {
    ret = mtl_rx_dequeue(s, &u, MTL_MS(100));
    if (ret == -MTL_EAGAIN) { /* no signal */
      ret = 0;
      continue;
    }
    if (ret < 0) break;
    const struct mtl_plane* p = &u.plane[0];
    uint32_t channels = p->row_bytes / 3, got = u.used / p->row_bytes; /* sample times */
    uint32_t frames = got * channels > cap ? cap / channels : got;
    for (uint32_t f = 0; f < frames; f++) {
      const uint8_t* b = (const uint8_t*)p->addr + (size_t)f * p->stride;
      for (uint32_t c = 0; c < channels; c++, b += 3) /* L24 big-endian, left-justified */
        pcm[f * channels + c] =
            (int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8);
    }
    int valid = (u.flags & MTL_UNITF_INDEX_VALID) != 0;
    int discont = u.missed_before > 0 || (u.flags & MTL_UNITF_DISCONTINUITY) ||
                  (valid && next >= 0 && u.media_index != next);
    int64_t first = valid ? u.media_index : -1;
    next = valid ? first + got : -1;
    ret = mtl_rx_release(s, u.lease); /* copied: release before the push */
    if (ret >= 0) ret = push_pcm(pcm, frames, first, discont);
  }
  return ret < 0 ? ex_fail("audio rx", ret) : 0;
}
