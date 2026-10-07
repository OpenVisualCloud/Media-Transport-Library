/* ex13 — audio as host PCM, both ways: the wire's sample layout and byte order, the index
   of the first sample, and the copy path. Sessions: MTL_AUDIO with MTL_PCM24. Needs: MS4
   (audio MS4a1; media_index valid from MS3). */
#include <mtl/experimental/mtl_util.h>

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
    next = valid ? first + frames : -1; /* samples cut by cap read as lost time next */
    ret = mtl_rx_release(s, u.lease);   /* copied: release before the push */
    if (ret >= 0) ret = push_pcm(pcm, frames, first, discont);
  }
  return ret < 0 ? ex_fail("audio rx", ret) : 0;
}

/* Host samples (a sound card's int32) to L24 big-endian in wire, then written: MTL stamps
   each write at the sample after the last (media mode AUTO). From MS6 a live source sets
   media mode TAI and how.media_tai_ns = its first sample's capture instant, and MTL
   absorbs up to a packet of jitter (audio.absorb_samples). wire: room for samples x 3
   bytes. */
int tx_audio(mtl_session_h s, const int32_t* pcm, uint32_t samples, uint8_t* wire) {
  size_t bytes = (size_t)samples * 3, done = 0;
  for (uint32_t i = 0; i < samples; i++) {
    uint32_t v = (uint32_t)pcm[i];
    wire[3 * i] = (uint8_t)(v >> 24);
    wire[3 * i + 1] = (uint8_t)(v >> 16);
    wire[3 * i + 2] = (uint8_t)(v >> 8);
  }
  while (done < bytes && g_running) { /* a full pool takes part: the rest goes next */
    int n = mtl_tx_write(s, wire + done, bytes - done, NULL, MTL_MS(20));
    if (n < 0 && n != -MTL_EAGAIN) return n;
    if (n > 0) done += (size_t)n;
  }
  return 0;
}

/* A monitor's meter under a picture: the sample of `audio` at which video unit `video`
   starts, from the two media indices and the exact rates, never from floored times.
   raster: the video session's. 0 = found (*offset), 1 = in a later audio unit, < 0 an
   error (-MTL_ERANGE: the picture starts before this audio unit). */
int audio_under_picture(const struct mtl_unit* video, const struct mtl_raster* raster,
                        const struct mtl_unit* audio, uint32_t* offset) {
  int64_t off;
  int ret = mtl_rx_align(video, raster, audio, 48000, &off);
  if (ret < 0) return ret;
  if (off >= (int64_t)(audio->used / audio->plane[0].row_bytes)) return 1;
  *offset = (uint32_t)off;
  return 0;
}

/* Fast metadata (ST 2110-41) takes the same copy path: one data item group per
   mtl_tx_write, used 0 the keep-alive; like ANC it follows the video of its start. */
void fastmeta_config(struct mtl_session_config* sc, uint32_t data_item_type) {
  MTL_INIT(sc);
  sc->direction = MTL_TX;
  sc->essence = MTL_FASTMETA;
  mtl_flow_ipv4(&sc->flows[0], 239, 168, 85, 70, 50000);
  sc->fastmeta.data_item_type = data_item_type;
}
