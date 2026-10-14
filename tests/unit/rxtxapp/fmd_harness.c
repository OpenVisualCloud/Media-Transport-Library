/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Compiles parse_json.c, tx_st40p_app.c, rx_st40p_app.c and rx_ancillary_app.c in
 * place. Fake lib calls and a fake clock take the place of the lib, so the real app
 * init, threads and result run without a NIC. The threads run on the test thread:
 * the fake get call stops them when the test input is used up.
 */

#include "rxtxapp/fmd_harness.h"

#include "../../tools/RxTxApp/src/parse_json.c"
#include "../../tools/RxTxApp/src/rx_ancillary_app.h"
#include "../../tools/RxTxApp/src/rx_st40p_app.h"
#include "../../tools/RxTxApp/src/tx_st40p_app.h"

#define UT_FB_CNT 3
#define UT_MAX_ITEMS 4096
#define UT_ITEM_BYTES 2048
/* st40p_tx_put_frame refuses a bigger fast metadata data item */
#define UT_FMD_MAX_ITEM 1436

struct ut_tx_st40p {
  struct st_app_context ctx;
  st_json_st40p_session_t json;
  struct st_app_tx_st40p_session s;
  struct st40p_tx_ops ops;
  struct st40_frame_info frames[UT_FB_CNT];
  int get_cnt;
  int put;
  int target;
  uint32_t fill[UT_MAX_ITEMS];
  uint8_t item[UT_MAX_ITEMS][UT_ITEM_BYTES];
};

struct ut_rx_item {
  struct st40_frame_info info;
  uint8_t data[UT_ITEM_BYTES];
  uint64_t at_ns;
};

struct ut_rx_st40p {
  struct st_app_context ctx;
  st_json_st40p_session_t json;
  struct st_app_rx_st40p_session s;
  struct st40p_rx_ops ops;
  struct ut_rx_item items[UT_MAX_ITEMS];
  int pushed;
  int next;
};

struct ut_rx_pkt {
  uint8_t data[UT_ITEM_BYTES];
  uint16_t len;
  uint64_t at_ns;
};

struct ut_rx_anc {
  struct st_app_context ctx;
  st_json_ancillary_session_t json;
  struct st_app_rx_anc_session s;
  struct ut_rx_pkt pkts[UT_MAX_ITEMS];
  int pushed;
  int next;
};

static uint64_t ut_now_ns;
static bool ut_rx_create_fail;
static struct ut_tx_st40p* ut_tx_cur;
static struct ut_rx_st40p* ut_rx_cur;
static struct ut_rx_anc* ut_anc_cur;
static st_json_interface_t ut_inf = {.name = "0000:00:00.0"};

static uint64_t ut_now(void) {
  return ut_now_ns;
}

static st40p_tx_handle ut_st40p_tx_create(mtl_handle mt, struct st40p_tx_ops* ops) {
  struct ut_tx_st40p* t = ut_tx_cur;
  (void)mt;
  t->ops = *ops;
  for (int i = 0; i < UT_FB_CNT; i++) {
    t->frames[i].udw_buff_addr = calloc(1, ops->max_udw_buff_size);
    t->frames[i].udw_buffer_size = ops->max_udw_buff_size;
  }
  return (st40p_tx_handle)t;
}

static int ut_st40p_tx_free(st40p_tx_handle handle) {
  struct ut_tx_st40p* t = (struct ut_tx_st40p*)handle;
  for (int i = 0; i < UT_FB_CNT; i++) {
    free(t->frames[i].udw_buff_addr);
    t->frames[i].udw_buff_addr = NULL;
  }
  return 0;
}

static size_t ut_st40p_tx_max_udw_buff_size(st40p_tx_handle handle) {
  return ((struct ut_tx_st40p*)handle)->ops.max_udw_buff_size;
}

static struct st40_frame_info* ut_st40p_tx_get_frame(st40p_tx_handle handle) {
  struct ut_tx_st40p* t = (struct ut_tx_st40p*)handle;
  if (t->put >= t->target) {
    t->s.st40p_app_thread_stop = true;
    return NULL;
  }
  return &t->frames[t->get_cnt++ % UT_FB_CNT];
}

/* the contract of st40p_tx_put_frame with ST40P_TX_FLAG_FAST_METADATA */
static int ut_st40p_tx_put_frame(st40p_tx_handle handle, struct st40_frame_info* f) {
  struct ut_tx_st40p* t = (struct ut_tx_st40p*)handle;
  uint32_t fill = f->udw_buffer_fill;

  if (t->put >= UT_MAX_ITEMS) return -ENOSPC;
  if (fill > f->udw_buffer_size || fill > UT_FMD_MAX_ITEM) return -EIO;
  memset(t->item[t->put], 0, UT_ITEM_BYTES);
  memcpy(t->item[t->put], f->udw_buff_addr, fill);
  t->fill[t->put] = fill;
  t->put++;
  return 0;
}

static int ut_st40p_tx_wake_block(st40p_tx_handle handle) {
  (void)handle;
  return 0;
}

static void* ut_mtl_hp_zmalloc(mtl_handle mt, size_t size, enum mtl_port port) {
  (void)mt;
  (void)port;
  return calloc(1, size);
}

static void ut_mtl_hp_free(mtl_handle mt, void* p) {
  (void)mt;
  free(p);
}

static st40p_rx_handle ut_st40p_rx_create(mtl_handle mt, struct st40p_rx_ops* ops) {
  (void)mt;
  ut_rx_cur->ops = *ops;
  if (ut_rx_create_fail) return NULL;
  return (st40p_rx_handle)ut_rx_cur;
}

static int ut_st40p_rx_free(st40p_rx_handle handle) {
  (void)handle;
  return 0;
}

static struct st40_frame_info* ut_st40p_rx_get_frame(st40p_rx_handle handle) {
  struct ut_rx_st40p* r = (struct ut_rx_st40p*)handle;
  if (r->next >= r->pushed) {
    r->s.st40p_app_thread_stop = true;
    return NULL;
  }
  struct ut_rx_item* item = &r->items[r->next++];
  ut_now_ns = item->at_ns;
  return &item->info;
}

static int ut_st40p_rx_put_frame(st40p_rx_handle handle, struct st40_frame_info* f) {
  (void)handle;
  (void)f;
  return 0;
}

static int ut_st40p_rx_wake_block(st40p_rx_handle handle) {
  (void)handle;
  return 0;
}

static int ut_st40p_rx_get_session_stats(st40p_rx_handle handle,
                                         struct st40_rx_user_stats* stats) {
  (void)handle;
  (void)stats;
  return -EIO;
}

static st40_rx_handle ut_st40_rx_create(mtl_handle mt, struct st40_rx_ops* ops) {
  (void)mt;
  (void)ops;
  return (st40_rx_handle)ut_anc_cur;
}

static int ut_st40_rx_free(st40_rx_handle handle) {
  (void)handle;
  return 0;
}

static void* ut_st40_rx_get_mbuf(st40_rx_handle handle, void** usrptr, uint16_t* len) {
  struct ut_rx_anc* r = (struct ut_rx_anc*)handle;
  if (r->next >= r->pushed) {
    r->s.st40_app_thread_stop = true;
    return NULL;
  }
  struct ut_rx_pkt* pkt = &r->pkts[r->next++];
  ut_now_ns = pkt->at_ns;
  *usrptr = pkt->data;
  *len = pkt->len;
  return pkt;
}

static void ut_st40_rx_put_mbuf(st40_rx_handle handle, void* mbuf) {
  (void)handle;
  (void)mbuf;
}

/* the app threads run on the test thread, see the ut_*_run calls */
static int ut_pthread_create(pthread_t* tid, const pthread_attr_t* attr,
                             void* (*fn)(void*), void* arg) {
  (void)attr;
  (void)fn;
  (void)arg;
  *tid = (pthread_t)1;
  return 0;
}

static int ut_pthread_join(pthread_t tid, void** ret) {
  (void)tid;
  (void)ret;
  return 0;
}

static int ut_mtl_thread_setname(pthread_t tid, const char* name) {
  (void)tid;
  (void)name;
  return 0;
}

#define st_app_get_monotonic_time ut_now
#define st40p_tx_create ut_st40p_tx_create
#define st40p_tx_free ut_st40p_tx_free
#define st40p_tx_max_udw_buff_size ut_st40p_tx_max_udw_buff_size
#define st40p_tx_get_frame ut_st40p_tx_get_frame
#define st40p_tx_put_frame ut_st40p_tx_put_frame
#define st40p_tx_wake_block ut_st40p_tx_wake_block
#define mtl_hp_zmalloc ut_mtl_hp_zmalloc
#define mtl_hp_free ut_mtl_hp_free
#define st40p_rx_create ut_st40p_rx_create
#define st40p_rx_free ut_st40p_rx_free
#define st40p_rx_get_frame ut_st40p_rx_get_frame
#define st40p_rx_put_frame ut_st40p_rx_put_frame
#define st40p_rx_wake_block ut_st40p_rx_wake_block
#define st40p_rx_get_session_stats ut_st40p_rx_get_session_stats
#define st40_rx_create ut_st40_rx_create
#define st40_rx_free ut_st40_rx_free
#define st40_rx_get_mbuf ut_st40_rx_get_mbuf
#define st40_rx_put_mbuf ut_st40_rx_put_mbuf
#define pthread_create ut_pthread_create
#define pthread_join ut_pthread_join
#define mtl_thread_setname ut_mtl_thread_setname
#include "../../tools/RxTxApp/src/rx_ancillary_app.c"
#include "../../tools/RxTxApp/src/rx_st40p_app.c"
#include "../../tools/RxTxApp/src/tx_st40p_app.c"
#undef mtl_thread_setname
#undef pthread_join
#undef pthread_create
#undef st40_rx_put_mbuf
#undef st40_rx_get_mbuf
#undef st40_rx_free
#undef st40_rx_create
#undef st40p_rx_get_session_stats
#undef st40p_rx_wake_block
#undef st40p_rx_put_frame
#undef st40p_rx_get_frame
#undef st40p_rx_free
#undef st40p_rx_create
#undef mtl_hp_free
#undef mtl_hp_zmalloc
#undef st40p_tx_wake_block
#undef st40p_tx_put_frame
#undef st40p_tx_get_frame
#undef st40p_tx_max_udw_buff_size
#undef st40p_tx_free
#undef st40p_tx_create
#undef st_app_get_monotonic_time

/* one session of an RxTxApp json file, as st_app_parse_json fills it */
static int ut_parse_st40p_session(const char* json, bool tx,
                                  st_json_st40p_session_t* st40p) {
  json_object* obj = json_tokener_parse(json);
  if (!obj) return -ST_JSON_PARSE_FAIL;
  memset(st40p, 0, sizeof(*st40p));
  st40p->base.inf[MTL_SESSION_PORT_P] = &ut_inf;
  st40p->base.num_inf = 1;
  int ret =
      tx ? st_json_parse_tx_st40p(0, obj, st40p) : st_json_parse_rx_st40p(0, obj, st40p);
  json_object_put(obj);
  return ret;
}

static int ut_parse_anc_session(const char* json, bool tx,
                                st_json_ancillary_session_t* anc) {
  json_object* obj = json_tokener_parse(json);
  if (!obj) return -ST_JSON_PARSE_FAIL;
  memset(anc, 0, sizeof(*anc));
  anc->base.inf[MTL_SESSION_PORT_P] = &ut_inf;
  anc->base.num_inf = 1;
  int ret = tx ? st_json_parse_tx_anc(0, obj, anc) : st_json_parse_rx_anc(0, obj, anc);
  json_object_put(obj);
  return ret;
}

void ut_parse_st40p(const char* json, bool tx, struct ut_fmd_json* out) {
  st_json_st40p_session_t st40p;
  memset(out, 0, sizeof(*out));
  out->ret = ut_parse_st40p_session(json, tx, &st40p);
  out->fast_metadata = st40p.info.fast_metadata;
  out->dit = st40p.info.fmd_dit;
  out->k_bit = st40p.info.fmd_k_bit;
  out->payload_type = st40p.base.payload_type;
  (void)snprintf(out->url, sizeof(out->url), "%s", st40p.info.st40p_url);
}

void ut_parse_anc(const char* json, bool tx, struct ut_fmd_json* out) {
  st_json_ancillary_session_t anc;
  memset(out, 0, sizeof(*out));
  out->ret = ut_parse_anc_session(json, tx, &anc);
  out->fast_metadata = anc.info.fast_metadata;
  out->dit = anc.info.fmd_dit;
  out->k_bit = anc.info.fmd_k_bit;
  out->payload_type = anc.base.payload_type;
  (void)snprintf(out->url, sizeof(out->url), "%s", anc.info.anc_url);
}

void ut_rxtxapp_set_now(uint64_t ns) {
  ut_now_ns = ns;
}

ut_tx_st40p* ut_tx_st40p_create(const char* json) {
  ut_tx_st40p* t = calloc(1, sizeof(*t));
  if (!t) return NULL;
  if (ut_parse_st40p_session(json, true, &t->json) < 0) {
    free(t);
    return NULL;
  }
  (void)snprintf(t->s.st40p_source_url, sizeof(t->s.st40p_source_url), "%s",
                 t->json.info.st40p_url);
  ut_tx_cur = t;
  if (app_tx_st40p_init(&t->ctx, &t->json, &t->s) < 0) {
    free(t);
    return NULL;
  }
  return t;
}

uint32_t ut_tx_st40p_ops_flags(ut_tx_st40p* t) {
  return t->ops.flags;
}

int ut_tx_st40p_run(ut_tx_st40p* t, int n) {
  t->target = t->put + n;
  t->s.st40p_app_thread_stop = false;
  app_tx_st40p_frame_thread(&t->s);
  return t->put;
}

uint32_t ut_tx_st40p_item(ut_tx_st40p* t, int i, uint8_t* out, uint32_t cap) {
  uint32_t padded = (t->fill[i] + 3) & ~3u;
  if (padded > cap) return 0;
  memcpy(out, t->item[i], padded);
  return padded;
}

uint32_t ut_tx_st40p_fill(ut_tx_st40p* t, int i) {
  return t->fill[i];
}

void ut_tx_st40p_free(ut_tx_st40p* t) {
  if (!t) return;
  app_tx_st40p_uinit(&t->s);
  free(t);
}

ut_rx_st40p* ut_rx_st40p_create(const char* json) {
  ut_rx_st40p* r = calloc(1, sizeof(*r));
  if (!r) return NULL;
  if (ut_parse_st40p_session(json, false, &r->json) < 0) {
    free(r);
    return NULL;
  }
  ut_rx_cur = r;
  if (app_rx_st40p_init(&r->ctx, &r->json, &r->s) < 0) {
    free(r);
    return NULL;
  }
  return r;
}

uint32_t ut_rx_st40p_ops_flags(ut_rx_st40p* r) {
  return r->ops.flags;
}

void ut_rx_st40p_fail_create(bool fail) {
  ut_rx_create_fail = fail;
}

bool ut_rx_st40p_ref_mapped(ut_rx_st40p* r) {
  return r->s.st40p_fmd_ref.data != NULL;
}

void ut_rx_st40p_push(ut_rx_st40p* r, const uint8_t* item, uint32_t fill, uint32_t dit,
                      uint8_t k_bit, bool marker, bool seq_discont, uint32_t seq_lost,
                      uint64_t at_ns) {
  if (r->pushed >= UT_MAX_ITEMS || fill > UT_ITEM_BYTES) return;
  struct ut_rx_item* it = &r->items[r->pushed++];
  memset(it, 0, sizeof(*it));
  memcpy(it->data, item, fill);
  it->info.udw_buff_addr = it->data;
  it->info.udw_buffer_size = UT_ITEM_BYTES;
  it->info.udw_buffer_fill = fill;
  it->info.fmd_dit = dit;
  it->info.fmd_k_bit = k_bit;
  it->info.rtp_marker = marker;
  it->info.seq_discont = seq_discont;
  it->info.seq_lost = seq_lost;
  it->info.status = ST_FRAME_STATUS_COMPLETE;
  it->at_ns = at_ns;
}

void ut_rx_st40p_run(ut_rx_st40p* r) {
  r->s.st40p_app_thread_stop = false;
  app_rx_st40p_frame_thread(&r->s);
}

int ut_rx_st40p_received(ut_rx_st40p* r) {
  return r->s.stat_frame_total_received;
}

int ut_rx_st40p_invalid(ut_rx_st40p* r) {
  return r->s.stat_frame_invalid;
}

int ut_rx_st40p_result(ut_rx_st40p* r) {
  int ret = app_rx_st40p_result(&r->s);
  (void)fflush(stdout);
  return ret;
}

void ut_rx_st40p_free(ut_rx_st40p* r) {
  if (!r) return;
  app_rx_st40p_uinit(&r->s);
  free(r);
}

ut_rx_anc* ut_rx_anc_create(const char* json) {
  ut_rx_anc* r = calloc(1, sizeof(*r));
  if (!r) return NULL;
  if (ut_parse_anc_session(json, false, &r->json) < 0) {
    free(r);
    return NULL;
  }
  ut_anc_cur = r;
  if (app_rx_anc_init(&r->ctx, &r->json, &r->s) < 0) {
    free(r);
    return NULL;
  }
  return r;
}

void ut_rx_anc_push(ut_rx_anc* r, uint16_t seq, const uint8_t* item, uint16_t padded,
                    uint64_t at_ns) {
  struct st40_fmd_rtp_hdr hdr;

  if (r->pushed >= UT_MAX_ITEMS || sizeof(hdr) + padded > UT_ITEM_BYTES) return;
  struct ut_rx_pkt* pkt = &r->pkts[r->pushed++];
  memset(&hdr, 0, sizeof(hdr));
  hdr.base.version = 2;
  hdr.base.payload_type = r->json.base.payload_type;
  hdr.base.seq_number = htons(seq);
  hdr.fmd_hdr_chunk.data_item_length = padded / 4;
  hdr.fmd_hdr_chunk.data_item_k_bit = r->json.info.fmd_k_bit;
  hdr.fmd_hdr_chunk.data_item_type = r->json.info.fmd_dit;
  hdr.swapped_fmd_hdr_chunk = htonl(hdr.swapped_fmd_hdr_chunk);
  memcpy(pkt->data, &hdr, sizeof(hdr));
  memcpy(pkt->data + sizeof(hdr), item, padded);
  pkt->len = sizeof(hdr) + padded;
  pkt->at_ns = at_ns;
}

void ut_rx_anc_run(ut_rx_anc* r) {
  r->s.st40_app_thread_stop = false;
  app_rx_anc_read_thread(&r->s);
}

int ut_rx_anc_invalid(ut_rx_anc* r) {
  return r->s.stat_pkt_invalid;
}

int ut_rx_anc_result(ut_rx_anc* r) {
  int ret = app_rx_anc_result(&r->s);
  (void)fflush(stdout);
  return ret;
}

void ut_rx_anc_free(ut_rx_anc* r) {
  if (!r) return;
  app_rx_anc_uinit(&r->s);
  free(r);
}
