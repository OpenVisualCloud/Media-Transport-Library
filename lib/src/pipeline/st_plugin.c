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

#include "st_plugin.h"

#include <dlfcn.h>

#include "../st_log.h"

static inline struct st_plugin_mgr* st_get_plugins_mgr(struct st_main_impl* impl) {
  return &impl->plugin_mgr;
}

static int st_plugin_free(struct st_dl_plugin_impl* plugin) {
  if (plugin->free) plugin->free(plugin->handle);
  if (plugin->dl_handle) {
    dlclose(plugin->dl_handle);
    plugin->dl_handle = NULL;
  }
  st_rte_free(plugin);

  return 0;
}

int st_plugins_init(struct st_main_impl* impl) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);

  st_pthread_mutex_init(&mgr->lock, NULL);
  st_pthread_mutex_init(&mgr->plugins_lock, NULL);

  info("%s, succ\n", __func__);
  return 0;
}

int st_plugins_uinit(struct st_main_impl* impl) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);

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
      st_rte_free(mgr->encode_devs[i]);
      mgr->encode_devs[i] = NULL;
    }
  }
  for (int i = 0; i < ST_MAX_DECODER_DEV; i++) {
    if (mgr->decode_devs[i]) {
      dbg("%s, still has decode dev in %d\n", __func__, i);
      st_rte_free(mgr->decode_devs[i]);
      mgr->decode_devs[i] = NULL;
    }
  }
  st_pthread_mutex_destroy(&mgr->lock);
  st_pthread_mutex_destroy(&mgr->plugins_lock);

  return 0;
}

int st22_encode_notify_frame_ready(struct st22_encode_session_impl* encoder) {
  struct st22_encode_dev_impl* dev_impl = encoder->parnet;
  struct st22_encoder_dev* dev = &dev_impl->dev;
  st22_encode_priv session = encoder->session;

  return dev->notify_frame_available(session);
}

int st22_put_encoder(struct st_main_impl* impl,
                     struct st22_encode_session_impl* encoder) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_encode_dev_impl* dev_impl = encoder->parnet;
  struct st22_encoder_dev* dev = &dev_impl->dev;
  int idx = dev_impl->idx;
  st22_encode_priv session = encoder->session;

  st_pthread_mutex_lock(&mgr->lock);
  dev->free_session(dev->priv, session);
  encoder->session = NULL;
  rte_atomic32_dec(&dev_impl->ref_cnt);
  st_pthread_mutex_unlock(&mgr->lock);

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

  for (int i = 0; i < ST_MAX_SESSIIONS_PER_ENCODER; i++) {
    session_impl = &dev_impl->sessions[i];
    if (session_impl->session) continue;

    session = dev->create_session(dev->priv, session_impl, create_req);
    if (session) {
      session_impl->session = session;
      session_impl->codestream_max_size = create_req->max_codestream_size;
      session_impl->req = *req;
      session_impl->type = ST22_SESSION_TYPE_PIPELINE_ENCODE;
      info("%s(%d), get one session at %d on dev %s, max codestream size %ld\n", __func__,
           idx, i, dev->name, session_impl->codestream_max_size);
      info("%s(%d), input fmt: %s, output fmt: %s\n", __func__, idx,
           st_frame_fmt_name(req->req.input_fmt), st_frame_fmt_name(req->req.output_fmt));
      return session_impl;
    } else {
      return NULL;
    }
  }

  return NULL;
}

static bool st22_encoder_is_capable(struct st22_encoder_dev* dev,
                                    struct st22_get_encoder_request* req) {
  enum st22_codec codec = req->codec;
  enum st_plugin_device plugin_dev = req->device;

  if (codec != dev->codec) return false;

  if ((plugin_dev != ST_PLUGIN_DEVICE_AUTO) && (plugin_dev != dev->target_device))
    return false;

  if (!(ST_BIT64(req->req.input_fmt) & dev->input_fmt_caps)) return false;

  if (!(ST_BIT64(req->req.output_fmt) & dev->output_fmt_caps)) return false;

  return true;
}

struct st22_encode_session_impl* st22_get_encoder(struct st_main_impl* impl,
                                                  struct st22_get_encoder_request* req) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_encoder_dev* dev;
  struct st22_encode_dev_impl* dev_impl;
  struct st22_encode_session_impl* session_impl;

  st_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_ENCODER_DEV; i++) {
    dev_impl = mgr->encode_devs[i];
    if (!dev_impl) continue;
    dbg("%s(%d), try to find one dev\n", __func__, i);
    dev = &mgr->encode_devs[i]->dev;
    if (!st22_encoder_is_capable(dev, req)) continue;

    dbg("%s(%d), try to find one session\n", __func__, i);
    session_impl = st22_get_encoder_session(dev_impl, req);
    if (session_impl) {
      rte_atomic32_inc(&dev_impl->ref_cnt);
      st_pthread_mutex_unlock(&mgr->lock);
      return session_impl;
    }
  }
  st_pthread_mutex_unlock(&mgr->lock);

  err("%s, fail to find one encode session\n", __func__);
  return NULL;
}

int st22_decode_notify_frame_ready(struct st22_decode_session_impl* decoder) {
  struct st22_decode_dev_impl* dev_impl = decoder->parnet;
  struct st22_decoder_dev* dev = &dev_impl->dev;
  st22_decode_priv session = decoder->session;

  return dev->notify_frame_available(session);
}

int st22_put_decoder(struct st_main_impl* impl,
                     struct st22_decode_session_impl* decoder) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_decode_dev_impl* dev_impl = decoder->parnet;
  struct st22_decoder_dev* dev = &dev_impl->dev;
  int idx = dev_impl->idx;
  st22_decode_priv session = decoder->session;

  st_pthread_mutex_lock(&mgr->lock);
  dev->free_session(dev->priv, session);
  decoder->session = NULL;
  rte_atomic32_dec(&dev_impl->ref_cnt);
  st_pthread_mutex_unlock(&mgr->lock);

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

  for (int i = 0; i < ST_MAX_SESSIIONS_PER_DECODER; i++) {
    session_impl = &dev_impl->sessions[i];
    if (session_impl->session) continue;

    session = dev->create_session(dev->priv, session_impl, create_req);
    if (session) {
      session_impl->session = session;
      session_impl->req = *req;
      session_impl->type = ST22_SESSION_TYPE_PIPELINE_DECODE;
      info("%s(%d), get one session at %d on dev %s\n", __func__, idx, i, dev->name);
      info("%s(%d), input fmt: %s, output fmt: %s\n", __func__, idx,
           st_frame_fmt_name(req->req.input_fmt), st_frame_fmt_name(req->req.output_fmt));
      return session_impl;
    } else {
      return NULL;
    }
  }

  return NULL;
}

static bool st22_decoder_is_capable(struct st22_decoder_dev* dev,
                                    struct st22_get_decoder_request* req) {
  enum st22_codec codec = req->codec;
  enum st_plugin_device plugin_dev = req->device;

  if (codec != dev->codec) return false;

  if ((plugin_dev != ST_PLUGIN_DEVICE_AUTO) && (plugin_dev != dev->target_device))
    return false;

  if (!(ST_BIT64(req->req.input_fmt) & dev->input_fmt_caps)) return false;

  if (!(ST_BIT64(req->req.output_fmt) & dev->output_fmt_caps)) return false;

  return true;
}

struct st22_decode_session_impl* st22_get_decoder(struct st_main_impl* impl,
                                                  struct st22_get_decoder_request* req) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_decoder_dev* dev;
  struct st22_decode_dev_impl* dev_impl;
  struct st22_decode_session_impl* session_impl;

  st_pthread_mutex_lock(&mgr->lock);
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
      st_pthread_mutex_unlock(&mgr->lock);
      return session_impl;
    }
  }
  st_pthread_mutex_unlock(&mgr->lock);

  err("%s, fail to find one decode session\n", __func__);
  return NULL;
}

static int st22_encode_dev_dump(struct st22_encode_dev_impl* encode) {
  struct st22_encode_session_impl* session;
  int ref_cnt = rte_atomic32_read(&encode->ref_cnt);

  if (ref_cnt) info("ST22 encoder dev %s with %d sessons\n", encode->name, ref_cnt);
  for (int i = 0; i < ST_MAX_SESSIIONS_PER_ENCODER; i++) {
    session = &encode->sessions[i];
    if (!session->session) continue;
    if (session->req.dump) session->req.dump(session->req.priv);
  }

  return 0;
}

static int st22_decode_dev_dump(struct st22_decode_dev_impl* decode) {
  struct st22_decode_session_impl* session;
  int ref_cnt = rte_atomic32_read(&decode->ref_cnt);

  if (ref_cnt) info("ST22 encoder dev %s with %d sessons\n", decode->name, ref_cnt);
  for (int i = 0; i < ST_MAX_SESSIIONS_PER_DECODER; i++) {
    session = &decode->sessions[i];
    if (!session->session) continue;
    if (session->req.dump) session->req.dump(session->req.priv);
  }

  return 0;
}

int st_plugins_dump(struct st_main_impl* impl) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_encode_dev_impl* encode;
  struct st22_decode_dev_impl* decode;

  st_pthread_mutex_lock(&mgr->lock);
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
  st_pthread_mutex_unlock(&mgr->lock);

  return 0;
}

int st22_encoder_unregister(st22_encoder_dev_handle handle) {
  struct st22_encode_dev_impl* dev = handle;

  if (dev->type != ST22_SESSION_TYPE_DEV_ENCODE) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  struct st_main_impl* impl = dev->parnet;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  int idx = dev->idx;

  if (mgr->encode_devs[idx] != dev) {
    err("%s, invalid dev %p\n", __func__, dev);
    return -EIO;
  }

  info("%s(%d), unregister %s\n", __func__, idx, dev->name);
  st_pthread_mutex_lock(&mgr->lock);
  int ref_cnt = rte_atomic32_read(&dev->ref_cnt);
  if (ref_cnt) {
    st_pthread_mutex_unlock(&mgr->lock);
    err("%s(%d), %s are busy with ref_cnt %d\n", __func__, idx, dev->name, ref_cnt);
    return -EBUSY;
  }
  st_rte_free(dev);
  mgr->encode_devs[idx] = NULL;
  st_pthread_mutex_unlock(&mgr->lock);

  return 0;
}

int st22_decoder_unregister(st22_decoder_dev_handle handle) {
  struct st22_decode_dev_impl* dev = handle;

  if (dev->type != ST22_SESSION_TYPE_DEV_DECODE) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  struct st_main_impl* impl = dev->parnet;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  int idx = dev->idx;

  if (mgr->decode_devs[idx] != dev) {
    err("%s, invalid dev %p\n", __func__, dev);
    return -EIO;
  }

  info("%s(%d), unregister %s\n", __func__, idx, dev->name);
  st_pthread_mutex_lock(&mgr->lock);
  int ref_cnt = rte_atomic32_read(&dev->ref_cnt);
  if (ref_cnt) {
    st_pthread_mutex_unlock(&mgr->lock);
    err("%s(%d), %s are busy with ref_cnt %d\n", __func__, idx, dev->name, ref_cnt);
    return -EBUSY;
  }
  st_rte_free(dev);
  mgr->decode_devs[idx] = NULL;
  st_pthread_mutex_unlock(&mgr->lock);

  return 0;
}

st22_encoder_dev_handle st22_encoder_register(st_handle st,
                                              struct st22_encoder_dev* dev) {
  struct st_main_impl* impl = st;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_encode_dev_impl* encode_dev;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
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

  st_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_ENCODER_DEV; i++) {
    if (mgr->encode_devs[i]) continue;
    encode_dev =
        st_rte_zmalloc_socket(sizeof(*encode_dev), st_socket_id(impl, ST_PORT_P));
    if (!encode_dev) {
      st_pthread_mutex_unlock(&mgr->lock);
      return NULL;
    }
    encode_dev->type = ST22_SESSION_TYPE_DEV_ENCODE;
    encode_dev->parnet = impl;
    encode_dev->idx = i;
    rte_atomic32_set(&encode_dev->ref_cnt, 0);
    strncpy(encode_dev->name, dev->name, ST_MAX_NAME_LEN - 1);
    encode_dev->dev = *dev;
    for (int j = 0; j < ST_MAX_SESSIIONS_PER_ENCODER; j++) {
      encode_dev->sessions[j].idx = j;
      encode_dev->sessions[j].parnet = encode_dev;
    }
    mgr->encode_devs[i] = encode_dev;
    st_pthread_mutex_unlock(&mgr->lock);
    info("%s(%d), %s registered, device %d cap(0x%lx:0x%lx)\n", __func__, i,
         encode_dev->name, dev->target_device, dev->input_fmt_caps, dev->output_fmt_caps);
    return encode_dev;
  }
  st_pthread_mutex_unlock(&mgr->lock);

  err("%s, no space, all items are used\n", __func__);
  return NULL;
}

st22_decoder_dev_handle st22_decoder_register(st_handle st,
                                              struct st22_decoder_dev* dev) {
  struct st_main_impl* impl = st;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st22_decode_dev_impl* decode_dev;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
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

  st_pthread_mutex_lock(&mgr->lock);
  for (int i = 0; i < ST_MAX_DECODER_DEV; i++) {
    if (mgr->decode_devs[i]) continue;
    decode_dev =
        st_rte_zmalloc_socket(sizeof(*decode_dev), st_socket_id(impl, ST_PORT_P));
    if (!decode_dev) {
      st_pthread_mutex_unlock(&mgr->lock);
      return NULL;
    }
    decode_dev->type = ST22_SESSION_TYPE_DEV_DECODE;
    decode_dev->parnet = impl;
    decode_dev->idx = i;
    rte_atomic32_set(&decode_dev->ref_cnt, 0);
    strncpy(decode_dev->name, dev->name, ST_MAX_NAME_LEN - 1);
    decode_dev->dev = *dev;
    for (int j = 0; j < ST_MAX_SESSIIONS_PER_DECODER; j++) {
      decode_dev->sessions[j].idx = j;
      decode_dev->sessions[j].parnet = decode_dev;
    }
    mgr->decode_devs[i] = decode_dev;
    st_pthread_mutex_unlock(&mgr->lock);
    info("%s(%d), %s registered, device %d cap(0x%lx:0x%lx)\n", __func__, i,
         decode_dev->name, dev->target_device, dev->input_fmt_caps, dev->output_fmt_caps);
    return decode_dev;
  }
  st_pthread_mutex_unlock(&mgr->lock);

  err("%s, no space, all items are used\n", __func__);
  return NULL;
}

struct st22_encode_frame_meta* st22_encoder_get_frame(st22p_encode_session session) {
  struct st22_encode_session_impl* session_impl = session;

  if (session_impl->type != ST22_SESSION_TYPE_PIPELINE_ENCODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return NULL;
  }

  return session_impl->req.get_frame(session_impl->req.priv);
}

int st22_encoder_put_frame(st22p_encode_session session,
                           struct st22_encode_frame_meta* frame, int result) {
  struct st22_encode_session_impl* session_impl = session;

  if (session_impl->type != ST22_SESSION_TYPE_PIPELINE_ENCODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return -EIO;
  }

  return session_impl->req.put_frame(session_impl->req.priv, frame, result);
}

struct st22_decode_frame_meta* st22_decoder_get_frame(st22p_decode_session session) {
  struct st22_decode_session_impl* session_impl = session;

  if (session_impl->type != ST22_SESSION_TYPE_PIPELINE_DECODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return NULL;
  }

  return session_impl->req.get_frame(session_impl->req.priv);
}

int st22_decoder_put_frame(st22p_decode_session session,
                           struct st22_decode_frame_meta* frame, int result) {
  struct st22_decode_session_impl* session_impl = session;

  if (session_impl->type != ST22_SESSION_TYPE_PIPELINE_DECODE) {
    err("%s(%d), invalid type %d\n", __func__, session_impl->idx, session_impl->type);
    return -EIO;
  }

  return session_impl->req.put_frame(session_impl->req.priv, frame, result);
}

static struct st_dl_plugin_impl* st_plugin_by_path(struct st_main_impl* impl,
                                                   const char* path) {
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st_dl_plugin_impl* plugin;

  st_pthread_mutex_lock(&mgr->plugins_lock);
  for (int i = 0; i < ST_MAX_DL_PLUGINS; i++) {
    plugin = mgr->plugins[i];
    if (plugin) {
      if (!strncmp(plugin->path, path, ST_PLUNGIN_MAX_PATH_LEN - 1)) {
        dbg("%s, %s registered\n", __func__, path);
        st_pthread_mutex_unlock(&mgr->plugins_lock);
        return plugin;
      }
    }
  }
  st_pthread_mutex_unlock(&mgr->plugins_lock);

  return NULL;
}

int st_get_plugins_nb(st_handle st) {
  struct st_main_impl* impl = st;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return 0;
  }

  return mgr->plugins_nb;
}

int st_plugin_register(st_handle st, const char* path) {
  struct st_main_impl* impl = st;
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

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  /* open the plugin and parse the symble */
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
  pl_handle = create_fn(st);
  if (!pl_handle) {
    err("%s, create_fn run fail in %s\n", __func__, path);
    dlclose(dl_handle);
    return -EIO;
  }

  struct st_dl_plugin_impl* plugin;
  /* add to the plugins */
  st_pthread_mutex_lock(&mgr->plugins_lock);
  for (int i = 0; i < ST_MAX_DL_PLUGINS; i++) {
    if (mgr->plugins[i]) continue;
    plugin = st_rte_zmalloc_socket(sizeof(*plugin), st_socket_id(impl, ST_PORT_P));
    if (!plugin) {
      st_pthread_mutex_unlock(&mgr->plugins_lock);
      dlclose(dl_handle);
      return -ENOMEM;
    }
    plugin->idx = i;
    strncpy(plugin->path, path, ST_PLUNGIN_MAX_PATH_LEN - 1);
    plugin->dl_handle = dl_handle;
    plugin->create = create_fn;
    plugin->free = free_fn;
    plugin->handle = pl_handle;
    plugin->meta = meta;
    mgr->plugins_nb++;
    mgr->plugins[i] = plugin;
    st_pthread_mutex_unlock(&mgr->plugins_lock);
    info("%s(%d), %s registered, version %d\n", __func__, i, path, meta.version);
    return 0;
  }
  st_pthread_mutex_unlock(&mgr->plugins_lock);

  err("%s, no space, all items are used\n", __func__);
  dlclose(dl_handle);
  return -EIO;
}

int st_plugin_unregister(st_handle st, const char* path) {
  struct st_main_impl* impl = st;
  struct st_plugin_mgr* mgr = st_get_plugins_mgr(impl);
  struct st_dl_plugin_impl* plugin;

  st_pthread_mutex_lock(&mgr->plugins_lock);
  for (int i = 0; i < ST_MAX_DL_PLUGINS; i++) {
    plugin = mgr->plugins[i];
    if (plugin) {
      if (!strncmp(plugin->path, path, ST_PLUNGIN_MAX_PATH_LEN - 1)) {
        info("%s, unregister %s at %d\n", __func__, path, i);
        st_plugin_free(plugin);
        mgr->plugins[i] = NULL;
        st_pthread_mutex_unlock(&mgr->plugins_lock);
        return 0;
      }
    }
  }
  st_pthread_mutex_unlock(&mgr->plugins_lock);
  err("%s, can not find %s\n", __func__, path);
  return -EIO;
}
