/* ex17 — observe a running instance: its log lines, every session's stats, its events,
   and a packet capture on request, read on threads of its own. Needs: MS4 (capture;
   events MS3; the rest MS2). */
#include <mtl/experimental/mtl_events.h>
#include <mtl/experimental/mtl_observe.h>

#include "ex_common.h"

#define DESCS 256
#define VALUES 2048
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

/* One scrape: the schema once, then every value in one snapshot; a schema that grew (a
   leg, a reason seen first) fails the read with -MTL_ESTALE, and the scrape lists again.
   A histogram exports its count (values[first]); its buckets follow. */
static int scrape_one(mtl_session_h s, const char* name) {
  static struct mtl_stat_desc d[DESCS];
  static int64_t v[VALUES];
  struct mtl_object o = MTL_OBJ_OF_SESSION(s);
  int ret = -MTL_ESTALE;
  for (int tries = 0; ret == -MTL_ESTALE && tries < 3; tries++) {
    uint32_t n = 0, count = 0;
    uint64_t gen = 0;
    ret = mtl_stat_list(o, d, sizeof(d[0]), DESCS, &n, &gen);
    if (ret < 0) return ret;
    if (n > DESCS) n = DESCS;
    if (n) count = d[n - 1].first + d[n - 1].width;
    ret = count > VALUES ? -MTL_ENOSPC : mtl_stat_read(o, gen, 0, count, v, NULL);
    for (uint32_t i = 0; ret >= 0 && i < n; i++)
      prom(name, d[i].name, d[i].label, v[d[i].first]);
  }
  return ret < 0 ? ret : 0;
}
int scrape(mtl_instance_h mt) {
  mtl_session_h s[64];
  struct mtl_session_info info;
  uint32_t n = 0;
  int ret = mtl_instance_list_sessions(mt, s, 64, &n); /* closing ones included */
  for (uint32_t i = 0; ret >= 0 && i < n && i < 64; i++) {
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
  while (g_running) {
    int n = mtl_instance_read_events(mt, ev, 8, MTL_SEC(1));
    if (n == -MTL_EAGAIN) continue;
    if (n < 0) return n == -MTL_ECANCELED || n == -MTL_ESHUTDOWN ? 0 : n;
    for (int i = 0; i < n; i++) export_event(&ev[i]);
  }
  return 0;
}

/* The capture button: the session's packets on every leg to a pcapng file, written by a
   library worker; MTL_EVENT_CAPTURE_DONE on the session when max_pkts are in. */
int capture(mtl_session_h s, const char* path) {
  struct mtl_capture_params p;
  MTL_INIT(&p);
  p.max_pkts = 100000;
  p.path = path;
  return mtl_session_capture(s, &p);
}
