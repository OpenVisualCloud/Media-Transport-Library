/* ex13 — audio as host PCM, both ways: the wire's sample layout and byte order, the index
   of the first sample, and the copy path. Sessions: MTL_AUDIO with MTL_PCM24. Needs:
   MS4. */
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

/* The framework's push: frames of interleaved int32 samples; first = the epoch index of
   the first sample (-1 = unknown); discont = 1 after lost time (GStreamer DISCONT). */
int push_pcm(const int32_t* pcm, uint32_t frames, int64_t first, int discont);

/* pcm: room for info.unit_samples sample frames of every channel. */
int rx_audio(mtl_session_h s, int32_t* pcm) {
  struct mtl_unit u;
  int64_t next = -1; /* the sample expected next */
  int ret;
  MTL_INIT(&u);
  while ((ret = mtl_rx_dequeue(s, &u, MTL_FOREVER)) == 0) {
    const struct mtl_plane* p = &u.plane[0]; /* a row per sample frame */
    uint32_t channels = p->row_bytes / 3, frames = u.used / p->row_bytes;
    for (uint32_t f = 0; f < frames; f++)
      mtl_pcm24_to_s32(pcm + (size_t)f * channels,
                       (const uint8_t*)p->addr + (size_t)f * p->stride, channels);
    int64_t first = (u.flags & MTL_UNITF_INDEX_VALID) ? u.media_index : -1;
    int discont = u.missed_before > 0 || (u.flags & MTL_UNITF_DISCONTINUITY) ||
                  (first >= 0 && next >= 0 && first != next);
    next = first >= 0 ? first + frames : -1;
    ret = mtl_rx_release(s, u.lease); /* copied: release before the push */
    if (ret >= 0) ret = push_pcm(pcm, frames, first, discont);
    if (ret < 0) break;
  }
  return ret == -MTL_ECANCELED ? 0 : ex_fail("audio rx", ret);
}

/* Host samples (a sound card's int32) to L24 in wire, then written: in media mode AUTO,
   MTL stamps each unit at the sample after the last. wire: room for samples x 3 bytes.
   Fast metadata (ST 2110-41: essence MTL_FASTMETA, fastmeta.data_item_type) takes the
   same copy path, one data item group per write. */
int tx_audio(mtl_session_h s, const int32_t* pcm, uint32_t samples, uint8_t* wire) {
  mtl_s32_to_pcm24(wire, pcm, samples);
  int n = mtl_tx_write(s, wire, (size_t)samples * 3, NULL, MTL_FOREVER);
  return n < 0 ? n : 0; /* fewer bytes only after an error, which the next call returns */
}

/* A monitor's meter under a picture: the sample of `audio` at which video unit `video`
   starts, from the two media indices and the exact rates, never from floored times.
   raster: the video session's. 0 = found (*offset), 1 = in a later audio unit, < 0 an
   error (-MTL_ERANGE: the picture starts before this audio unit). */
int audio_under_picture(const struct mtl_unit* video, const struct mtl_raster* raster,
                        const struct mtl_unit* audio, uint32_t sample_rate,
                        uint32_t* offset) {
  int64_t off;
  int ret = mtl_rx_align(video, raster, audio, sample_rate, &off);
  if (ret < 0) return ret;
  if (off >= (int64_t)(audio->used / audio->plane[0].row_bytes)) return 1;
  *offset = (uint32_t)off;
  return 0;
}
