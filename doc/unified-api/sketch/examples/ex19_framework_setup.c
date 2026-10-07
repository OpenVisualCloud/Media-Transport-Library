/* ex19 — a framework element's setup, before media flows: options as properties, one
   shared instance, caps as formats, and the latency before the session exists. Needs:
   MS2. */
#include <mtl/experimental/mtl_format.h>
#include <mtl/experimental/mtl_options.h>

#include "ex_common.h"

#define KEYS 512
void install_property(const struct mtl_option_desc* d); /* GObject, AVOption */

/* class_init: a property per session key: type, range, default, enum names. */
int install_properties(void) {
  static struct mtl_option_desc d[KEYS];
  uint32_t n = 0;
  int ret = mtl_option_list(d, sizeof(d[0]), KEYS, &n);
  for (uint32_t i = 0; ret >= 0 && i < n && i < KEYS; i++)
    if (d[i].object_kind == MTL_OBJ_SESSION) install_property(&d[i]);
  return ret < 0 ? ret : 0;
}

/* set_property("rx.skew_budget_ns", "5ms"), "tx.index_offset" = "-2" (a lip-sync trim),
   "port.dhcp/1" = "on": parsed now and kept for the next create (a string value points
   into `value`); instance and port keys go to mtl_instance_params.options. Returns the
   key's object kind. */
int remember_for_create(const char* name, const char* value, struct mtl_option* opts,
                        uint32_t* n) {
  int kind = mtl_option_parse(name, value, &opts[*n]); /* -MTL_EINVAL names the key */
  if (kind >= 0) (*n)++;
  return kind;
}
/* A session key on a running session: keys marked R change at once, the others are
   -MTL_EBUSY (OPTION_STATE) until the next create. */
int apply_now(mtl_session_h s, const struct mtl_option* o) {
  return mtl_set_option(MTL_OBJ_OF_SESSION(s), o);
}
/* get_property: the effective value, the derived default included. */
int get_property(mtl_session_h s, const char* name, int64_t* value) {
  struct mtl_option_desc d;
  int ret = mtl_option_find(name, &d, sizeof(d));
  return ret < 0 ? ret : mtl_get_option(MTL_OBJ_OF_SESSION(s), d.key, 0, value, NULL, 0);
}

/* start: every element of the process joins one instance (the first open creates it). */
#define MAX_PORTS 4
int join_instance(const char* ports, mtl_instance_h* mt) {
  struct mtl_port_spec spec[MAX_PORTS];
  struct mtl_instance_params p;
  uint32_t n = 0;
  /* built against these headers, run on an older libmtl: refuse */
  if (mtl_library_version_num() < MTL_API_VERSION) return -MTL_ENOTSUP;
  int ret = mtl_port_parse(ports, spec, MAX_PORTS, &n);
  if (ret < 0) return ret;
  MTL_INIT(&p);
  p.ports = spec;
  p.port_count = n < MAX_PORTS ? n : MAX_PORTS; /* n counts every port, also those cut */
  p.flags = MTL_INSTANCE_SHARED;
  ret = mtl_instance_open(&p, mt); /* -MTL_EEXIST: the process opened it otherwise */
  return ret < 0 ? ex_fail("instance", ret) : 0;
}

/* set_caps: "v210", "yuv422p10le" or "YCbCr-4:2:2/10" to the config, and the size of a
   frame the framework's buffers must hold. */
int caps_to_config(const char* format, uint32_t w, uint32_t h,
                   struct mtl_session_config* sc, uint64_t* frame_bytes) {
  struct mtl_format_desc d;
  uint32_t f = 0, kind = 0;
  int ret = mtl_format_parse(format, &f, &kind);
  if (ret >= 0) ret = mtl_format_describe(f, kind, w, h, &d, sizeof(d));
  if (ret < 0) return ret;
  /* an application format: MTL converts in dequeue or submit (rx.convert_per_packet:
     per packet, on the RX tasklet) */
  if (kind == MTL_FORMAT_APP)
    sc->video.app_format = f;
  else if (kind == MTL_FORMAT_TRANSPORT)
    sc->video.format = f;
  else
    return -MTL_EINVAL;
  sc->video.raster.width = w;
  sc->video.raster.height = h;
  *frame_bytes = 0; /* the planes of one frame */
  for (uint32_t i = 0; i < d.plane_count && i < MTL_MAX_PLANES; i++)
    *frame_bytes += d.plane_bytes[i];
  return 0;
}

/* GST_QUERY_LATENCY in READY: the dry run of create (also wire_bps, for admission); a
   sink adds the copy in its submit, info.convert_ns once the session exists. */
int query_latency(mtl_instance_h mt, const struct mtl_session_config* sc, int64_t* min_ns,
                  int64_t* max_ns) {
  struct mtl_session_info info;
  int ret = mtl_session_query(mt, sc, 0, &info, sizeof(info), NULL, 0);
  if (ret < 0) return ret;
  *min_ns = info.latency_min_ns;
  *max_ns = info.latency_max_ns;
  return 0;
}
