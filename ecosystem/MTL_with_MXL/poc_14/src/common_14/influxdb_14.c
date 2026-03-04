/* SPDX-License-Identifier: BSD-3-Clause
 * poc_14 — InfluxDB v2 push with stream_id tag
 *
 * Separate from poc/influxdb.c to avoid modifying it.
 * Uses its own static CURL handle.
 */

#include "influxdb_14.h"
#include "poc_latency.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <curl/curl.h>

/* ── Module state ── */
static bool     s_enabled = false;
static CURL    *s_curl    = NULL;
static char     s_url[512];
static char     s_auth_header[512];
static char     s_host_tag[128];

static size_t discard_cb(void *p, size_t sz, size_t n, void *ud)
{
    (void)p; (void)ud;
    return sz * n;
}

bool poc14_influxdb_init(void)
{
    const char *url    = getenv("INFLUXDB_URL");
    const char *token  = getenv("INFLUXDB_TOKEN");
    const char *org    = getenv("INFLUXDB_ORG");
    const char *bucket = getenv("INFLUXDB_BUCKET");
    const char *host   = getenv("POC_HOST");

    if (!url || !token) {
        s_enabled = false;
        return false;
    }
    if (!org)    org    = "mtl-mxl";
    if (!bucket) bucket = "poc14";

    if (host) {
        snprintf(s_host_tag, sizeof(s_host_tag), "%s", host);
    } else {
        gethostname(s_host_tag, sizeof(s_host_tag));
        s_host_tag[sizeof(s_host_tag) - 1] = '\0';
    }

    snprintf(s_url, sizeof(s_url),
             "%s/api/v2/write?org=%s&bucket=%s&precision=ns",
             url, org, bucket);
    snprintf(s_auth_header, sizeof(s_auth_header), "Token %s", token);

    /* Use curl_easy_init without curl_global_init — the original
     * poc influxdb.c may have already called it. curl handles
     * the ref-counting internally. */
    s_curl = curl_easy_init();
    if (!s_curl) {
        fprintf(stderr, "[influxdb_14] curl_easy_init failed\n");
        s_enabled = false;
        return false;
    }

    s_enabled = true;
    fprintf(stderr, "[influxdb_14] enabled → %s  host=%s\n", s_url, s_host_tag);
    return true;
}

void poc14_influxdb_push(poc_stats_t *stats, const char *role, int stream_id)
{
    if (!s_enabled || !s_curl)
        return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ts_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    /* Snapshot latencies then reset windows so max/min/avg are per-period */
    poc_latency_t lq = stats->lat_queue;
    poc_latency_t lb = stats->lat_bridge;
    poc_latency_t lr = stats->lat_rdma;
    poc_latency_t lt = stats->lat_total;
    poc_latency_t lc = stats->lat_consume;
    poc_latency_t le = stats->lat_e2e;

    poc_latency_reset(&stats->lat_queue);
    poc_latency_reset(&stats->lat_bridge);
    poc_latency_reset(&stats->lat_rdma);
    poc_latency_reset(&stats->lat_total);
    poc_latency_reset(&stats->lat_consume);
    poc_latency_reset(&stats->lat_e2e);

    char body[16384];
    int off = 0;

    /* Counters — with stream_id tag */
    off += snprintf(body + off, sizeof(body) - off,
        "poc_counters,role=%s,host=%s,stream_id=%d "
        "rx_frames=%luu,"
        "rx_incomplete=%luu,"
        "rx_drops=%luu,"
        "bridge_frames=%luu,"
        "bridge_copies=%luu,"
        "fabrics_transfers=%luu,"
        "target_events=%luu,"
        "consumer_frames=%luu"
        " %lu\n",
        role, s_host_tag, stream_id,
        (unsigned long)atomic_load(&stats->rx_frames),
        (unsigned long)atomic_load(&stats->rx_incomplete),
        (unsigned long)atomic_load(&stats->rx_drops),
        (unsigned long)atomic_load(&stats->bridge_frames),
        (unsigned long)atomic_load(&stats->bridge_copies),
        (unsigned long)atomic_load(&stats->fabrics_transfers),
        (unsigned long)atomic_load(&stats->target_events),
        (unsigned long)atomic_load(&stats->consumer_frames),
        (unsigned long)ts_ns);

    /* Latency stages — with stream_id tag + percentiles */
    struct { const char *name; const poc_latency_t *lat; } stages[] = {
        {"queue",    &lq}, {"bridge",   &lb}, {"rdma",     &lr},
        {"total",    &lt}, {"consume",  &lc},
        {"e2e",      &le},
    };

    uint64_t scratch[POC_LATENCY_SAMPLES_MAX];

    for (size_t i = 0; i < sizeof(stages)/sizeof(stages[0]); i++) {
        const poc_latency_t *l = stages[i].lat;
        if (l->count == 0) continue;

        uint64_t p50 = poc_latency_percentile(l, scratch, 50);
        uint64_t p95 = poc_latency_percentile(l, scratch, 95);
        uint64_t p99 = poc_latency_percentile(l, scratch, 99);

        off += snprintf(body + off, sizeof(body) - off,
            "poc_latency,role=%s,host=%s,stream_id=%d,stage=%s "
            "min=%luu,max=%luu,avg=%luu,p50=%luu,p95=%luu,p99=%luu,samples=%luu"
            " %lu\n",
            role, s_host_tag, stream_id, stages[i].name,
            (unsigned long)(l->min_us == UINT64_MAX ? 0 : l->min_us),
            (unsigned long)l->max_us,
            (unsigned long)poc_latency_avg(l),
            (unsigned long)p50,
            (unsigned long)p95,
            (unsigned long)p99,
            (unsigned long)l->count,
            (unsigned long)ts_ns);
    }

    /* POST to InfluxDB */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: text/plain; charset=utf-8");
    char auth[600];
    snprintf(auth, sizeof(auth), "Authorization: %s", s_auth_header);
    headers = curl_slist_append(headers, auth);

    curl_easy_setopt(s_curl, CURLOPT_URL, s_url);
    curl_easy_setopt(s_curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(s_curl, CURLOPT_POSTFIELDSIZE, (long)off);
    curl_easy_setopt(s_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(s_curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(s_curl, CURLOPT_TIMEOUT, 2L);

    curl_easy_perform(s_curl);
    curl_slist_free_all(headers);
}

void poc14_influxdb_cleanup(void)
{
    if (s_curl) {
        curl_easy_cleanup(s_curl);
        s_curl = NULL;
    }
    s_enabled = false;
}
