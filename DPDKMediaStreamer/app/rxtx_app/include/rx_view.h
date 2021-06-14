/*
* Copyright (C) 2020-2021 Intel Corporation.
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

#ifndef _RX_VIEW_H
#define _RX_VIEW_H

#include "st_api.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <pthread.h>

#define ST_DEFAULT_ANCILIARY "closed_captions.txt"
#define ST_DEFAULT_VIDEO_YUV "signal_be.yuv"
#define ST_DEFAULT_VIDEO_RGBA "signal_8b.rgba"
#define ST_DEFAULT_AUDIO "kahawai_sample_audio_ducks_PCM_16bit_48kHz.wav"

struct video_stream_info;
typedef struct video_stream_info video_stream_info_t;
struct audio_ref;
typedef struct audio_ref audio_ref_t;
extern st_status_t CreateGui(void);
struct anc_ref;
typedef struct anc_ref anc_ref_t;

extern st_status_t AddStream(video_stream_info_t **videoStream, const char *label,
							 st21_buf_fmt_t bufFormat, int width, int height);

extern st_status_t ShowFrame(video_stream_info_t *stream, uint8_t const *frame, int interlaced);

void DestroyGui(void);

bool DoesGuiExist(void);

st_status_t CreateAudioRef(audio_ref_t **ref);

st_status_t CreateAncRef(anc_ref_t **ref);

st_status_t PlayAudioFrame(audio_ref_t *ref, uint8_t const *frame, uint32_t frameSize);

st_status_t PlayAncFrame(anc_ref_t *ref, uint8_t const *frame, uint32_t frameSize);

char *AudioRefSelectFile(uint8_t bufFormat);

char *AncRefSelectFile(uint8_t bufFormat);

st_status_t AudioRefOpenFile(audio_ref_t *ref, const char *fileName);

st_status_t AncRefOpenFile(anc_ref_t *ref, const char *fileName);

st_status_t CreateGuiWindow(void);

#endif	//_RX_VIEW_H
