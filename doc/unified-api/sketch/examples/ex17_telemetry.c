/* ex17 — observe a running instance: its log lines, every session's stats and its
   events, read on threads of its own. Needs: MS3. */
#include <mtl/experimental/mtl_events.h>
#include <mtl/experimental/mtl_observe.h>

#include "ex_common.h"

#define DESCS 256
#define VALUES 2048
#define MAX_SESSIONS 64
void app_log(uint32_t severity, const char* origin, const char* text, uint32_t dropped);
void prom(const char* session, const char* name, const char* label, int64_t value);
void export_event(const struct mtl_event* e);

/* On the library's one log thread, one line at a time; it may call MTL, but not open,
   close or shut down an instance. */
static void log_line(void* priv, const struct mtl_log_record* r, size_t r_size) {
  (void)priv;
  (void)r_size;
  app_log(r->severity, r->origin_name, r->text, r->dropped);
}
int route_logs(mtl_log_sink_h* sink) { /* before open: EAL's lines come too */
  struct mtl_log_sink_params p;
  MTL_INIT(&p);
  p.min_severity = MTL_SEV_INFO;
  p.fn = log_line;
  return mtl_log_add_sink(&p, sink); /* mtl_log_remove_sink() before priv is freed */
}

/* One scrape: the schema, then every value in one snapshot. A histogram exports its
   count (values[first]); its buckets follow. */
static int scrape_one(mtl_session_h s, const char* name) {
  static struct mtl_stat_desc d[DESCS];
  static int64_t v[VALUES];
  struct mtl_object o = MTL_OBJ_OF_SESSION(s);
  uint32_t n = 0;
  int ret;
  for (int tries = 0; tries < 3; tries++) {
    uint32_t count = 0;
    uint64_t gen = 0;
    ret = mtl_stat_list(o, d, sizeof(d[0]), DESCS, &n, &gen);
    if (ret < 0) return ret;
    if (n > DESCS) n = DESCS;
    /* values are laid out in schema order: the last descriptor ends the array */
    if (n) count = d[n - 1].first + d[n - 1].width;
    if (count > VALUES) return -MTL_ENOSPC;
    ret = mtl_stat_read(o, gen, 0, count, v, NULL);
    if (ret != -MTL_ESTALE) break; /* the schema grew (a leg, a reason seen first) */
  }
  for (uint32_t i = 0; ret >= 0 && i < n; i++)
    prom(name, d[i].name, d[i].label, v[d[i].first]);
  return ret < 0 ? ret : 0;
}
int scrape(mtl_instance_h mt) {
  mtl_session_h s[MAX_SESSIONS];
  struct mtl_session_info info;
  uint32_t n = 0;
  int ret = mtl_instance_list_sessions(mt, s, MAX_SESSIONS, &n); /* closing ones too */
  for (uint32_t i = 0; ret >= 0 && i < n && i < MAX_SESSIONS; i++) {
    if (mtl_session_get_info(s[i], &info, sizeof(info)) < 0)
      continue; /* closed meanwhile */
    ret = scrape_one(s[i], info.name);
  }
  return ret < 0 ? ex_fail("scrape", ret) : 0;
}

/* One value by name, for an alert rule. */
int frames_too_late(mtl_session_h s, int64_t* n) {
  return mtl_stat_get(MTL_OBJ_OF_SESSION(s), "tx.units_dropped{reason=too_late}", n);
}

/* The instance's own events (ports, time, schedulers, health, MtlManager), on a thread of
   its own; MTL_EVENT_OVERFLOW: some were lost, read the getters again. */
int watch(mtl_instance_h mt) {
  struct mtl_event ev[8];
  int n;
  while ((n = mtl_instance_read_events(mt, ev, 8, MTL_FOREVER)) > 0)
    for (int i = 0; i < n; i++) export_event(&ev[i]);
  return n == -MTL_ECANCELED || n == -MTL_ESHUTDOWN ? 0 : n;
}
