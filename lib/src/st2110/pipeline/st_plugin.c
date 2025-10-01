/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_plugin.h"

#include <dlfcn.h>

#include "../../mt_log.h"
#include "../../mt_stat.h"

static int st_plugins_dump(void* priv);

static inline struct st_plugin_mgr* st_get_plugins_mgr(struct mtl_main_impl* impl) {
  return &impl->plugin_mgr;
}

static int st_plugin_free(struct st_dl_plugin_impl* plugin) {
  if (plugin->free) plugin->free(plugin->handle);
  if (plugin->dl_handle) {
    dlclose(plugin->dl_handle);
    plugin->dl_handle = NULL;
  }
  mt_rte_free(plugin);

  return 0;
}

int st_plugins_init(struct mtl_main_impl* impl) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);

  mt_pthread_mutex_init(&mgr->lock, NULL);
  mt_pthread_mutex_init(&mgr->plugins_lock, NULL);
  mt_stat_register(impl, st_plugins_dump, impl, "plugins");

  info("%s, succ\n", __func__);
  return 0;
}

int st_plugins_uinit(struct mtl_main_impl* impl) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);

  mt_stat_unregister(impl, st_plugins_dump, impl);
  for (int i = 0; i < ST_MAX_DL_PLUGINS; i++) {
    if (mgr->plugins[i]) {
      dbg("%s, active plugin in %d\n", __func__, i);
      st_plugin_free(mgr->plugins[i]);
      mgr->plugins[i] = NULL;
    }
  }
  for (int i = 0; i < ST_MAX_ENCODER_DEV; i++) {
    if (mgr->encode_devs[i]) {
      dbg("%s, still has encode dev in %d\n", __func__, i);
      mt_rte_free(mgr->encode_devs[i]);
      mgr->encode_devs[i] = NULL;
    }
  }
  for (int i = 0; i < ST_MAX_DECODER_DEV; i++) {
    if (mgr->decode_devs[i]) {
      dbg("%s, still has decode dev in %d\n", __func__, i);
      mt_rte_free(mgr->decode_devs[i]);
      mgr->decode_devs[i] = NULL;
    }
  }
  for (int i = 0; i < ST_MAX_CONVERTER_DEV; i++) {
    if (mgr->convert_devs[i]) {
      dbg("%s, still has convert dev in %d\n", __func__, i);
      mt_rte_free(mgr->convert_devs[i]);
      mgr->convert_devs[i] = NULL;
    }
  }
  mt_pthread_mutex_destroy(&mgr->lock);
  mt_pthread_mutex_destroy(&mgr->plugins_lock);

  return 0;
}

int st22_put_encoder(struct mtl_main_impl* impl,
                     struct st22_encode_session_impl* encoder) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_encode_dev_impl* dev_impl = encoder->parent;
  struct st22_encoder_dev* dev = &dev_impl->dev;
  int idx = dev_impl->idx;
  st22_encode_priv session = encoder->session;

  mt_pthread_mutex_lock(&mgr->lock);
  dev->free_session(dev->priv, session);
  encoder->session = NULL;
  rte_atomic32_dec(&dev_impl->ref_cnt);
  mt_pthread_mutex_unlock(&mgr->lock);

  info("%s(%d), put session %d succ\n", __func__, idx, encoder->idx);
  return 0;
}

static struct st22_encode_session_impl* st22_get_encoder_session(
    struct st22_encode_dev_impl* dev_impl, struct st22_get_encoder_request* req) {
  struct st22_encoder_dev* dev = &dev_impl->dev;
  int idx = dev_impl->idx;
  struct st22_encoder_create_req* create_req = &req->req;
  struct st22_encode_session_impl* session_impl;
  st22_encode_priv session;

  for (int i = 0; i < ST_MAX_SESSIONS_PER_ENCODER; i++) {
    session_impl = &dev_impl->sessions[i];
    if (session_impl->session) continue;

    session = dev->create_session(dev->priv, session_impl, create_req);
    if (session) {
      session_impl->session = session;
      session_impl->codestream_max_size = create_req->max_codestream_size;
      session_impl->req = *req;
      session_impl->type = MT_ST22_HANDLE_PIPELINE_ENCODE;
      info("%s(%d), get one session at %d on dev %s, max codestream size %" PRIu64 "\n",
           __func__, idx, i, dev->name, session_impl->codestream_max_size);
      info("%s(%d), input fmt: %s, output fmt: %s\n", __func__, idx,
           st_frame_fmt_name(req->req.input_fmt), st_frame_fmt_name(req->req.output_fmt));
      return session_impl;
    } else {
      err("%s(%d), fail to create one session at %d on dev %s\n", __func__, idx, i,
          dev->name);
      return NULL;
    }
  }

  return NULL;
}

static bool st22_encoder_is_capable(struct st22_encoder_dev* dev,
                                    struct st22_get_encoder_request* req) {
  enum st_plugin_device plugin_dev = req->device;

  if ((plugin_dev != ST_PLUGIN_DEVICE_AUTO) && (plugin_dev != dev->target_device))
    return false;

  if (!(MTL_BIT64(req->req.input_fmt) & dev->input_fmt_caps)) return false;

  if (!(MTL_BIT64(req->req.output_fmt) & dev->output_fmt_caps)) return false;

  return true;
}

struct st22_encode_session_impl* st22_get_encoder(struct mtl_main_impl* impl,
                                                  struct st22_get_encoder_request* req) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_encoder_dev* dev;
  struct st22_encode_dev_impl* dev_impl;
  struct st22_encode_session_impl* session_impl;

  mt_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_ENCODER_DEV; i++) {
    dev_impl = mgr->encode_devs[i];
    if (!dev_impl) continue;
    dbg("%s(%d), try to find one dev\n", __func__, i);
    dev = &mgr->encode_devs[i]->dev;
    if (!st22_encoder_is_capable(dev, req)) {
      dbg("%s(%d), %s not capable\n", __func__, i, dev->name);
      continue;
    }

    dbg("%s(%d), try to find one session\n", __func__, i);
    session_impl = st22_get_encoder_session(dev_impl, req);
    if (session_impl) {
      rte_atomic32_inc(&dev_impl->ref_cnt);
      mt_pthread_mutex_unlock(&mgr->lock);
      return session_impl;
    }
  }
  mt_pthread_mutex_unlock(&mgr->lock);

  err("%s, fail to get, input fmt: %s, output fmt: %s\n", __func__,
      st_frame_fmt_name(req->req.input_fmt), st_frame_fmt_name(req->req.output_fmt));
  return NULL;
}

int st22_put_decoder(struct mtl_main_impl* impl,
                     struct st22_decode_session_impl* decoder) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_decode_dev_impl* dev_impl = decoder->parent;
  struct st22_decoder_dev* dev = &dev_impl->dev;
  int idx = dev_impl->idx;
  st22_decode_priv session = decoder->session;

  mt_pthread_mutex_lock(&mgr->lock);
  dev->free_session(dev->priv, session);
  decoder->session = NULL;
  rte_atomic32_dec(&dev_impl->ref_cnt);
  mt_pthread_mutex_unlock(&mgr->lock);

  info("%s(%d), put session %d succ\n", __func__, idx, decoder->idx);
  return 0;
}

static struct st22_decode_session_impl* st22_get_decoder_session(
    struct st22_decode_dev_impl* dev_impl, struct st22_get_decoder_request* req) {
  struct st22_decoder_dev* dev = &dev_impl->dev;
  int idx = dev_impl->idx;
  struct st22_decoder_create_req* create_req = &req->req;
  struct st22_decode_session_impl* session_impl;
  st22_decode_priv session;

  for (int i = 0; i < ST_MAX_SESSIONS_PER_DECODER; i++) {
    session_impl = &dev_impl->sessions[i];
    if (session_impl->session) continue;

    session = dev->create_session(dev->priv, session_impl, create_req);
    if (session) {
      session_impl->session = session;
      session_impl->req = *req;
      session_impl->type = MT_ST22_HANDLE_PIPELINE_DECODE;
      info("%s(%d), get one session at %d on dev %s\n", __func__, idx, i, dev->name);
      info("%s(%d), input fmt: %s, output fmt: %s\n", __func__, idx,
           st_frame_fmt_name(req->req.input_fmt), st_frame_fmt_name(req->req.output_fmt));
      return session_impl;
    } else {
      err("%s(%d), fail to create one session at %d on dev %s\n", __func__, idx, i,
          dev->name);
      return NULL;
    }
  }

  return NULL;
}

static bool st22_decoder_is_capable(struct st22_decoder_dev* dev,
                                    struct st22_get_decoder_request* req) {
  enum st_plugin_device plugin_dev = req->device;

  if ((plugin_dev != ST_PLUGIN_DEVICE_AUTO) && (plugin_dev != dev->target_device))
    return false;

  if (!(MTL_BIT64(req->req.input_fmt) & dev->input_fmt_caps)) return false;

  if (!(MTL_BIT64(req->req.output_fmt) & dev->output_fmt_caps)) return false;

  return true;
}

struct st22_decode_session_impl* st22_get_decoder(struct mtl_main_impl* impl,
                                                  struct st22_get_decoder_request* req) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_decoder_dev* dev;
  struct st22_decode_dev_impl* dev_impl;
  struct st22_decode_session_impl* session_impl;

  mt_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_DECODER_DEV; i++) {
    dev_impl = mgr->decode_devs[i];
    if (!dev_impl) continue;
    dbg("%s(%d), try to find one dev\n", __func__, i);
    dev = &mgr->decode_devs[i]->dev;
    if (!st22_decoder_is_capable(dev, req)) continue;

    dbg("%s(%d), try to find one session\n", __func__, i);
    session_impl = st22_get_decoder_session(dev_impl, req);
    if (session_impl) {
      rte_atomic32_inc(&dev_impl->ref_cnt);
      mt_pthread_mutex_unlock(&mgr->lock);
      return session_impl;
    }
  }
  mt_pthread_mutex_unlock(&mgr->lock);

  err("%s, fail to get, input fmt: %s, output fmt: %s\n", __func__,
      st_frame_fmt_name(req->req.input_fmt), st_frame_fmt_name(req->req.output_fmt));
  return NULL;
}

int st20_convert_notify_frame_ready(struct st20_convert_session_impl* converter) {
  struct st20_convert_dev_impl* dev_impl = converter->parent;
  struct st20_converter_dev* dev = &dev_impl->dev;
  st20_convert_priv session = converter->session;

  return dev->notify_frame_available(session);
}

int st20_put_converter(struct mtl_main_impl* impl,
                       struct st20_convert_session_impl* converter) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st20_convert_dev_impl* dev_impl = converter->parent;
  struct st20_converter_dev* dev = &dev_impl->dev;
  int idx = dev_impl->idx;
  st20_convert_priv session = converter->session;

  mt_pthread_mutex_lock(&mgr->lock);
  dev->free_session(dev->priv, session);
  converter->session = NULL;
  rte_atomic32_dec(&dev_impl->ref_cnt);
  mt_pthread_mutex_unlock(&mgr->lock);

  info("%s(%d), put session %d succ\n", __func__, idx, converter->idx);
  return 0;
}

static struct st20_convert_session_impl* st20_get_converter_session(
    struct st20_convert_dev_impl* dev_impl, struct st20_get_converter_request* req) {
  struct st20_converter_dev* dev = &dev_impl->dev;
  int idx = dev_impl->idx;
  struct st20_converter_create_req* create_req = &req->req;
  struct st20_convert_session_impl* session_impl;
  st20_convert_priv session;

  for (int i = 0; i < ST_MAX_SESSIONS_PER_CONVERTER; i++) {
    session_impl = &dev_impl->sessions[i];
    if (session_impl->session) continue;

    session = dev->create_session(dev->priv, session_impl, create_req);
    if (session) {
      session_impl->session = session;
      session_impl->req = *req;
      session_impl->type = MT_ST20_HANDLE_PIPELINE_CONVERT;
      info("%s(%d), get one session at %d on dev %s\n", __func__, idx, i, dev->name);
      info("%s(%d), input fmt: %s, output fmt: %s\n", __func__, idx,
           st_frame_fmt_name(req->req.input_fmt), st_frame_fmt_name(req->req.output_fmt));
      return session_impl;
    } else {
      err("%s(%d), fail to create one session at %d on dev %s\n", __func__, idx, i,
          dev->name);
      return NULL;
    }
  }

  return NULL;
}

static bool st20_converter_is_capable(struct st20_converter_dev* dev,
                                      struct st20_get_converter_request* req) {
  enum st_plugin_device plugin_dev = req->device;

  if ((plugin_dev != ST_PLUGIN_DEVICE_AUTO) && (plugin_dev != dev->target_device))
    return false;

  if (!(MTL_BIT64(req->req.input_fmt) & dev->input_fmt_caps)) return false;

  if (!(MTL_BIT64(req->req.output_fmt) & dev->output_fmt_caps)) return false;

  return true;
}

struct st20_convert_session_impl* st20_get_converter(
    struct mtl_main_impl* impl, struct st20_get_converter_request* req) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st20_converter_dev* dev;
  struct st20_convert_dev_impl* dev_impl;
  struct st20_convert_session_impl* session_impl;

  mt_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_CONVERTER_DEV; i++) {
    dev_impl = mgr->convert_devs[i];
    if (!dev_impl) continue;
    dbg("%s(%d), try to find one dev\n", __func__, i);
    dev = &mgr->convert_devs[i]->dev;
    if (!st20_converter_is_capable(dev, req)) continue;

    dbg("%s(%d), try to find one session\n", __func__, i);
    session_impl = st20_get_converter_session(dev_impl, req);
    if (session_impl) {
      rte_atomic32_inc(&dev_impl->ref_cnt);
      mt_pthread_mutex_unlock(&mgr->lock);
      return session_impl;
    }
  }
  mt_pthread_mutex_unlock(&mgr->lock);

  info("%s, plugin not found, input fmt: %s, output fmt: %s\n", __func__,
       st_frame_fmt_name(req->req.input_fmt), st_frame_fmt_name(req->req.output_fmt));
  return NULL;
}

static int st22_encode_dev_dump(struct st22_encode_dev_impl* encode) {
  struct st22_encode_session_impl* session;
  int ref_cnt = rte_atomic32_read(&encode->ref_cnt);

  if (ref_cnt) notice("ST22 encoder dev: %s with %d sessions\n", encode->name, ref_cnt);
  for (int i = 0; i < ST_MAX_SESSIONS_PER_ENCODER; i++) {
    session = &encode->sessions[i];
    if (!session->session) continue;
    if (session->req.dump) session->req.dump(session->req.priv);
  }

  return 0;
}

static int st22_decode_dev_dump(struct st22_decode_dev_impl* decode) {
  struct st22_decode_session_impl* session;
  int ref_cnt = rte_atomic32_read(&decode->ref_cnt);

  if (ref_cnt) notice("ST22 encoder dev: %s with %d sessions\n", decode->name, ref_cnt);
  for (int i = 0; i < ST_MAX_SESSIONS_PER_DECODER; i++) {
    session = &decode->sessions[i];
    if (!session->session) continue;
    if (session->req.dump) session->req.dump(session->req.priv);
  }

  return 0;
}

static int st20_convert_dev_dump(struct st20_convert_dev_impl* convert) {
  struct st20_convert_session_impl* session;
  int ref_cnt = rte_atomic32_read(&convert->ref_cnt);

  if (ref_cnt) notice("ST20 convert dev: %s with %d sessions\n", convert->name, ref_cnt);
  for (int i = 0; i < ST_MAX_SESSIONS_PER_CONVERTER; i++) {
    session = &convert->sessions[i];
    if (!session->session) continue;
    if (session->req.dump) session->req.dump(session->req.priv);
  }

  return 0;
}

static int st_plugins_dump(void* priv) {
  struct mtl_main_impl* impl = priv;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_encode_dev_impl* encode;
  struct st22_decode_dev_impl* decode;
  struct st20_convert_dev_impl* convert;

  mt_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_ENCODER_DEV; i++) {
    encode = mgr->encode_devs[i];
    if (!encode) continue;
    st22_encode_dev_dump(encode);
  }
  for (int i = 0; i < ST_MAX_DECODER_DEV; i++) {
    decode = mgr->decode_devs[i];
    if (!decode) continue;
    st22_decode_dev_dump(decode);
  }
  for (int i = 0; i < ST_MAX_CONVERTER_DEV; i++) {
    convert = mgr->convert_devs[i];
    if (!convert) continue;
    st20_convert_dev_dump(convert);
  }
  mt_pthread_mutex_unlock(&mgr->lock);

  return 0;
}

int st22_encoder_unregister(st22_encoder_dev_handle handle) {
  struct st22_encode_dev_impl* dev = handle;

  if (dev->type != MT_ST22_HANDLE_DEV_ENCODE) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  struct mtl_main_impl* impl = dev->parent;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  int idx = dev->idx;

  if (mgr->encode_devs[idx] != dev) {
    err("%s, invalid dev %p\n", __func__, dev);
    return -EIO;
  }

  info("%s(%d), unregister %s\n", __func__, idx, dev->name);
  mt_pthread_mutex_lock(&mgr->lock);
  int ref_cnt = rte_atomic32_read(&dev->ref_cnt);
  if (ref_cnt) {
    mt_pthread_mutex_unlock(&mgr->lock);
    err("%s(%d), %s are busy with ref_cnt %d\n", __func__, idx, dev->name, ref_cnt);
    return -EBUSY;
  }
  mt_rte_free(dev);
  mgr->encode_devs[idx] = NULL;
  mt_pthread_mutex_unlock(&mgr->lock);

  return 0;
}

int st22_decoder_unregister(st22_decoder_dev_handle handle) {
  struct st22_decode_dev_impl* dev = handle;

  if (dev->type != MT_ST22_HANDLE_DEV_DECODE) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  struct mtl_main_impl* impl = dev->parent;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  int idx = dev->idx;

  if (mgr->decode_devs[idx] != dev) {
    err("%s, invalid dev %p\n", __func__, dev);
    return -EIO;
  }

  info("%s(%d), unregister %s\n", __func__, idx, dev->name);
  mt_pthread_mutex_lock(&mgr->lock);
  int ref_cnt = rte_atomic32_read(&dev->ref_cnt);
  if (ref_cnt) {
    mt_pthread_mutex_unlock(&mgr->lock);
    err("%s(%d), %s are busy with ref_cnt %d\n", __func__, idx, dev->name, ref_cnt);
    return -EBUSY;
  }
  mt_rte_free(dev);
  mgr->decode_devs[idx] = NULL;
  mt_pthread_mutex_unlock(&mgr->lock);

  return 0;
}

int st20_converter_unregister(st20_converter_dev_handle handle) {
  struct st20_convert_dev_impl* dev = handle;

  if (dev->type != MT_ST20_HANDLE_DEV_CONVERT) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  struct mtl_main_impl* impl = dev->parent;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  int idx = dev->idx;

  if (mgr->convert_devs[idx] != dev) {
    err("%s, invalid dev %p\n", __func__, dev);
    return -EIO;
  }

  info("%s(%d), unregister %s\n", __func__, idx, dev->name);
  mt_pthread_mutex_lock(&mgr->lock);
  int ref_cnt = rte_atomic32_read(&dev->ref_cnt);
  if (ref_cnt) {
    mt_pthread_mutex_unlock(&mgr->lock);
    err("%s(%d), %s are busy with ref_cnt %d\n", __func__, idx, dev->name, ref_cnt);
    return -EBUSY;
  }
  mt_rte_free(dev);
  mgr->convert_devs[idx] = NULL;
  mt_pthread_mutex_unlock(&mgr->lock);

  return 0;
}

st22_encoder_dev_handle st22_encoder_register(mtl_handle mt,
                                              struct st22_encoder_dev* dev) {
  struct mtl_main_impl* impl = mt;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_encode_dev_impl* encode_dev;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (!dev->create_session) {
    err("%s, pls set create_session\n", __func__);
    return NULL;
  }
  if (!dev->free_session) {
    err("%s, pls set free_session\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_ENCODER_DEV; i++) {
    if (mgr->encode_devs[i]) continue;
    encode_dev =
        mt_rte_zmalloc_socket(sizeof(*encode_dev), mt_socket_id(impl, MTL_PORT_P));
    if (!encode_dev) {
      err("%s, encode_dev malloc fail\n", __func__);
      mt_pthread_mutex_unlock(&mgr->lock);
      return NULL;
    }
    encode_dev->type = MT_ST22_HANDLE_DEV_ENCODE;
    encode_dev->parent = impl;
    encode_dev->idx = i;
    rte_atomic32_set(&encode_dev->ref_cnt, 0);
    snprintf(encode_dev->name, ST_MAX_NAME_LEN - 1, "%s", dev->name);
    encode_dev->dev = *dev;
    for (int j = 0; j < ST_MAX_SESSIONS_PER_ENCODER; j++) {
      encode_dev->sessions[j].idx = j;
      encode_dev->sessions[j].parent = encode_dev;
    }
    mgr->encode_devs[i] = encode_dev;
    mt_pthread_mutex_unlock(&mgr->lock);
    info("%s(%d), %s registered, device %d cap(0x%" PRIx64 ":0x%" PRIx64 ")\n", __func__,
         i, encode_dev->name, dev->target_device, dev->input_fmt_caps,
         dev->output_fmt_caps);
    return encode_dev;
  }
  mt_pthread_mutex_unlock(&mgr->lock);

  err("%s, no space, all items are used\n", __func__);
  return NULL;
}

st22_decoder_dev_handle st22_decoder_register(mtl_handle mt,
                                              struct st22_decoder_dev* dev) {
  struct mtl_main_impl* impl = mt;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_decode_dev_impl* decode_dev;

  if (!impl) {
    err("%s, invalid impl\n", __func__);
    return NULL;
  }

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (!dev->create_session) {
    err("%s, pls set create_session\n", __func__);
    return NULL;
  }
  if (!dev->free_session) {
    err("%s, pls set free_session\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_DECODER_DEV; i++) {
    if (mgr->decode_devs[i]) continue;
    decode_dev =
        mt_rte_zmalloc_socket(sizeof(*decode_dev), mt_socket_id(impl, MTL_PORT_P));
    if (!decode_dev) {
      err("%s, decode_dev malloc fail\n", __func__);
      mt_pthread_mutex_unlock(&mgr->lock);
      return NULL;
    }
    decode_dev->type = MT_ST22_HANDLE_DEV_DECODE;
    decode_dev->parent = impl;
    decode_dev->idx = i;
    rte_atomic32_set(&decode_dev->ref_cnt, 0);
    snprintf(decode_dev->name, ST_MAX_NAME_LEN - 1, "%s", dev->name);
    decode_dev->dev = *dev;
    for (int j = 0; j < ST_MAX_SESSIONS_PER_DECODER; j++) {
      decode_dev->sessions[j].idx = j;
      decode_dev->sessions[j].parent = decode_dev;
    }
    mgr->decode_devs[i] = decode_dev;
    mt_pthread_mutex_unlock(&mgr->lock);
    info("%s(%d), %s registered, device %d cap(0x%" PRIx64 ":0x%" PRIx64 ")\n", __func__,
         i, decode_dev->name, dev->target_device, dev->input_fmt_caps,
         dev->output_fmt_caps);
    return decode_dev;
  }
  mt_pthread_mutex_unlock(&mgr->lock);

  err("%s, no space, all items are used\n", __func__);
  return NULL;
}

st20_converter_dev_handle st20_converter_register(mtl_handle mt,
                                                  struct st20_converter_dev* dev) {
  struct mtl_main_impl* impl = mt;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st20_convert_dev_impl* convert_dev;

  if (!impl) {
    err("%s, null handle\n", __func__);
    return NULL;
  }

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (!dev->create_session) {
    err("%s, pls set create_session\n", __func__);
    return NULL;
  }
  if (!dev->free_session) {
    err("%s, pls set free_session\n", __func__);
    return NULL;
  }
  if (!dev->notify_frame_available) {
    err("%s, pls set notify_frame_available\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_CONVERTER_DEV; i++) {
    if (mgr->convert_devs[i]) continue;
    convert_dev =
        mt_rte_zmalloc_socket(sizeof(*convert_dev), mt_socket_id(impl, MTL_PORT_P));
    if (!convert_dev) {
      err("%s, convert_dev malloc fail\n", __func__);
      mt_pthread_mutex_unlock(&mgr->lock);
      return NULL;
    }
    convert_dev->type = MT_ST20_HANDLE_DEV_CONVERT;
    convert_dev->parent = impl;
    convert_dev->idx = i;
    rte_atomic32_set(&convert_dev->ref_cnt, 0);
    snprintf(convert_dev->name, ST_MAX_NAME_LEN - 1, "%s", dev->name);
    convert_dev->dev = *dev;
    for (int j = 0; j < ST_MAX_SESSIONS_PER_CONVERTER; j++) {
      convert_dev->sessions[j].idx = j;
      convert_dev->sessions[j].parent = convert_dev;
    }
    mgr->convert_devs[i] = convert_dev;
    mt_pthread_mutex_unlock(&mgr->lock);
    info("%s(%d), %s registered, device %d cap(0x%" PRIx64 ":0x%" PRIx64 ")\n", __func__,
         i, convert_dev->name, dev->target_device, dev->input_fmt_caps,
         dev->output_fmt_caps);
    return convert_dev;
  }
  mt_pthread_mutex_unlock(&mgr->lock);

  err("%s, no space, all items are used\n", __func__);
  return NULL;
}

struct st22_encode_frame_meta* st22_encoder_get_frame(st22p_encode_session session) {
  struct st22_encode_session_impl* session_impl = session;

  if (session_impl->type != MT_ST22_HANDLE_PIPELINE_ENCODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return NULL;
  }

  return session_impl->req.get_frame(session_impl->req.priv);
}

int st22_encoder_wake_block(st22p_encode_session session) {
  struct st22_encode_session_impl* session_impl = session;

  if (session_impl->type != MT_ST22_HANDLE_PIPELINE_ENCODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return -EIO;
  }

  return session_impl->req.wake_block(session_impl->req.priv);
}

int st22_encoder_set_block_timeout(st22p_encode_session session, uint64_t timedwait_ns) {
  struct st22_encode_session_impl* session_impl = session;

  if (session_impl->type != MT_ST22_HANDLE_PIPELINE_ENCODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return -EIO;
  }

  return session_impl->req.set_block_timeout(session_impl->req.priv, timedwait_ns);
}

int st22_encoder_put_frame(st22p_encode_session session,
                           struct st22_encode_frame_meta* frame, int result) {
  struct st22_encode_session_impl* session_impl = session;

  if (session_impl->type != MT_ST22_HANDLE_PIPELINE_ENCODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return -EIO;
  }

  return session_impl->req.put_frame(session_impl->req.priv, frame, result);
}

struct st22_decode_frame_meta* st22_decoder_get_frame(st22p_decode_session session) {
  struct st22_decode_session_impl* session_impl = session;

  if (session_impl->type != MT_ST22_HANDLE_PIPELINE_DECODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return NULL;
  }

  return session_impl->req.get_frame(session_impl->req.priv);
}

int st22_decoder_wake_block(st22p_decode_session session) {
  struct st22_decode_session_impl* session_impl = session;

  if (session_impl->type != MT_ST22_HANDLE_PIPELINE_DECODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return -EIO;
  }

  return session_impl->req.wake_block(session_impl->req.priv);
}

int st22_decoder_set_block_timeout(st22p_decode_session session, uint64_t timedwait_ns) {
  struct st22_decode_session_impl* session_impl = session;

  if (session_impl->type != MT_ST22_HANDLE_PIPELINE_DECODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return -EIO;
  }

  return session_impl->req.set_block_timeout(session_impl->req.priv, timedwait_ns);
}

int st22_decoder_put_frame(st22p_decode_session session,
                           struct st22_decode_frame_meta* frame, int result) {
  struct st22_decode_session_impl* session_impl = session;

  if (session_impl->type != MT_ST22_HANDLE_PIPELINE_DECODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return -EIO;
  }

  return session_impl->req.put_frame(session_impl->req.priv, frame, result);
}

struct st20_convert_frame_meta* st20_converter_get_frame(st20p_convert_session session) {
  struct st20_convert_session_impl* session_impl = session;

  if (session_impl->type != MT_ST20_HANDLE_PIPELINE_CONVERT) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return NULL;
  }

  return session_impl->req.get_frame(session_impl->req.priv);
}

int st20_converter_put_frame(st20p_convert_session session,
                             struct st20_convert_frame_meta* frame, int result) {
  struct st20_convert_session_impl* session_impl = session;

  if (session_impl->type != MT_ST20_HANDLE_PIPELINE_CONVERT) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return -EIO;
  }

  return session_impl->req.put_frame(session_impl->req.priv, frame, result);
}

static struct st_dl_plugin_impl* st_plugin_by_path(struct mtl_main_impl* impl,
                                                   const char* path) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st_dl_plugin_impl* plugin;

  mt_pthread_mutex_lock(&mgr->plugins_lock);
  for (int i = 0; i < ST_MAX_DL_PLUGINS; i++) {
    plugin = mgr->plugins[i];
    if (plugin) {
      if (!strncmp(plugin->path, path, ST_PLUGIN_MAX_PATH_LEN - 1)) {
        dbg("%s, %s registered\n", __func__, path);
        mt_pthread_mutex_unlock(&mgr->plugins_lock);
        return plugin;
      }
    }
  }
  mt_pthread_mutex_unlock(&mgr->plugins_lock);

  return NULL;
}

int st_get_plugins_nb(mtl_handle mt) {
  struct mtl_main_impl* impl = mt;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return 0;
  }

  return mgr->plugins_nb;
}

int st_plugin_register(mtl_handle mt, const char* path) {
  struct mtl_main_impl* impl = mt;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  void* dl_handle;
  st_plugin_get_meta_fn get_meta_fn;
  st_plugin_create_fn create_fn;
  st_plugin_free_fn free_fn;
  st_plugin_priv pl_handle;
  struct st_plugin_meta meta;
  int ret;

  if (st_plugin_by_path(impl, path)) {
    err("%s, %s already registered\n", __func__, path);
    return -EIO;
  }

  memset(&meta, 0, sizeof(meta));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  /* open the plugin and parse the symbol */
  dl_handle = dlopen(path, RTLD_LAZY);
  if (!dl_handle) {
    warn("%s, dlopen %s fail\n", __func__, path);
    return -EIO;
  }
  get_meta_fn = dlsym(dl_handle, ST_PLUGIN_GET_META_API);
  if (!get_meta_fn) {
    err("%s, no %s func in %s\n", __func__, ST_PLUGIN_GET_META_API, path);
    dlclose(dl_handle);
    return -EIO;
  }
  create_fn = dlsym(dl_handle, ST_PLUGIN_CREATE_API);
  if (!create_fn) {
    err("%s, no %s func in %s\n", __func__, ST_PLUGIN_CREATE_API, path);
    dlclose(dl_handle);
    return -EIO;
  }
  free_fn = dlsym(dl_handle, ST_PLUGIN_FREE_API);
  if (!free_fn) {
    err("%s, no %s func in %s\n", __func__, ST_PLUGIN_FREE_API, path);
    dlclose(dl_handle);
    return -EIO;
  }

  /* verify version */
  ret = get_meta_fn(&meta);
  if (ret < 0) {
    err("%s, get_meta_fn run fail in %s\n", __func__, path);
    dlclose(dl_handle);
    return -EIO;
  }
  if (meta.version == ST_PLUGIN_VERSION_V1) {
    if (meta.magic != ST_PLUGIN_VERSION_V1_MAGIC) {
      err("%s, error magic %u in %s\n", __func__, meta.magic, path);
      dlclose(dl_handle);
      return -EIO;
    }
  } else {
    err("%s, unknow version %d in %s\n", __func__, meta.version, path);
    dlclose(dl_handle);
    return -EIO;
  }
  pl_handle = create_fn(impl);
  if (!pl_handle) {
    err("%s, create_fn run fail in %s\n", __func__, path);
    dlclose(dl_handle);
    return -EIO;
  }

  struct st_dl_plugin_impl* plugin;
  /* add to the plugins */
  mt_pthread_mutex_lock(&mgr->plugins_lock);
  for (int i = 0; i < ST_MAX_DL_PLUGINS; i++) {
    if (mgr->plugins[i]) continue;
    plugin = mt_rte_zmalloc_socket(sizeof(*plugin), mt_socket_id(impl, MTL_PORT_P));
    if (!plugin) {
      mt_pthread_mutex_unlock(&mgr->plugins_lock);
      dlclose(dl_handle);
      return -ENOMEM;
    }
    plugin->idx = i;
    snprintf(plugin->path, ST_PLUGIN_MAX_PATH_LEN - 1, "%s", path);
    plugin->dl_handle = dl_handle;
    plugin->create = create_fn;
    plugin->free = free_fn;
    plugin->handle = pl_handle;
    plugin->meta = meta;
    mgr->plugins_nb++;
    mgr->plugins[i] = plugin;
    mt_pthread_mutex_unlock(&mgr->plugins_lock);
    info("%s(%d), %s registered, version %d\n", __func__, i, path, meta.version);
    return 0;
  }
  mt_pthread_mutex_unlock(&mgr->plugins_lock);

  err("%s, no space, all items are used\n", __func__);
  dlclose(dl_handle);
  return -EIO;
}

int st_plugin_unregister(mtl_handle mt, const char* path) {
  struct mtl_main_impl* impl = mt;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st_dl_plugin_impl* plugin;

  mt_pthread_mutex_lock(&mgr->plugins_lock);
  for (int i = 0; i < ST_MAX_DL_PLUGINS; i++) {
    plugin = mgr->plugins[i];
    if (plugin) {
      if (!strncmp(plugin->path, path, ST_PLUGIN_MAX_PATH_LEN - 1)) {
        info("%s, unregister %s at %d\n", __func__, path, i);
        st_plugin_free(plugin);
        mgr->plugins[i] = NULL;
        mt_pthread_mutex_unlock(&mgr->plugins_lock);
        return 0;
      }
    }
  }
  mt_pthread_mutex_unlock(&mgr->plugins_lock);
  err("%s, can not find %s\n", __func__, path);
  return -EIO;
}
