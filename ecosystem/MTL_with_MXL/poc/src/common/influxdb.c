/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — InfluxDB v2 line-protocol push via libcurl
 */

#include "poc_influxdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>

/* ── Module state ── */
static bool     s_enabled = false;
static CURL    *s_curl    = NULL;
static char     s_url[512];            /* full write-API URL */
static char     s_auth_header[512];    /* "Token <token>" */
static char     s_host_tag[128];       /* hostname tag value */

/* Discard response body */
static size_t discard_cb(void *p, size_t sz, size_t n, void *ud)
{
    (void)p; (void)ud;
    return sz * n;
}

bool poc_influxdb_init(void)
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
    if (!bucket) bucket = "poc";

    /* Derive host tag */
    if (host) {
        snprintf(s_host_tag, sizeof(s_host_tag), "%s", host);
    } else {
        gethostname(s_host_tag, sizeof(s_host_tag));
        s_host_tag[sizeof(s_host_tag) - 1] = '\0';
    }

    /* Build URL: /api/v2/write?org=...&bucket=...&precision=ns */
    snprintf(s_url, sizeof(s_url),
             "%s/api/v2/write?org=%s&bucket=%s&precision=ns",
             url, org, bucket);

    /* Auth header */
    snprintf(s_auth_header, sizeof(s_auth_header), "Token %s", token);

    curl_global_init(CURL_GLOBAL_ALL);
    s_curl = curl_easy_init();
    if (!s_curl) {
        fprintf(stderr, "[influxdb] curl_easy_init failed\n");
        s_enabled = false;
        return false;
    }

    s_enabled = true;
    fprintf(stderr, "[influxdb] enabled → %s  host=%s\n", s_url, s_host_tag);
    return true;
}

void poc_influxdb_push(const poc_stats_t *stats, const char *role)
{
    if (!s_enabled || !s_curl)
        return;

    /* NOTE: We intentionally omit timestamps from the line protocol.
     * InfluxDB will assign server-receive time.  This avoids clock-skew
     * issues when the receiver machine's CLOCK_REALTIME drifts from the
     * InfluxDB server (e.g. PTP not yet converged). */

    /* Snapshot latencies */
    poc_latency_t lq = stats->lat_queue;
    poc_latency_t lb = stats->lat_bridge;
    poc_latency_t lr = stats->lat_rdma;
    poc_latency_t lt = stats->lat_total;
    poc_latency_t lc = stats->lat_consume;
    poc_latency_t le = stats->lat_e2e;

    /* Build line-protocol body.
     * Measurement: poc_stats
     * Tags: role, host
     * Fields: all counters + latency triples
     *
     * We send one line per latency bucket plus one for counters
     * to keep field sets manageable.
     */
    char body[4096];
    int off = 0;

    /* Counters */
    off += snprintf(body + off, sizeof(body) - off,
        "poc_counters,role=%s,host=%s "
        "rx_frames=%luu,"
        "rx_incomplete=%luu,"
        "rx_drops=%luu,"
        "bridge_frames=%luu,"
        "bridge_copies=%luu,"
        "fabrics_transfers=%luu,"
        "target_events=%luu,"
        "consumer_frames=%luu"
        "\n",
        role, s_host_tag,
        (unsigned long)atomic_load(&stats->rx_frames),
        (unsigned long)atomic_load(&stats->rx_incomplete),
        (unsigned long)atomic_load(&stats->rx_drops),
        (unsigned long)atomic_load(&stats->bridge_frames),
        (unsigned long)atomic_load(&stats->bridge_copies),
        (unsigned long)atomic_load(&stats->fabrics_transfers),
        (unsigned long)atomic_load(&stats->target_events),
        (unsigned long)atomic_load(&stats->consumer_frames));

    /* Latency stages — with percentiles (p99) */
    struct { const char *name; const poc_latency_t *lat; } stages[] = {
        {"queue",   &lq}, {"bridge",  &lb}, {"rdma",    &lr},
        {"total",   &lt}, {"consume", &lc}, {"e2e",     &le},
    };

    uint64_t scratch[POC_LATENCY_SAMPLES_MAX];

    for (size_t i = 0; i < sizeof(stages)/sizeof(stages[0]); i++) {
        const poc_latency_t *l = stages[i].lat;
        if (l->count == 0) continue;

        uint64_t p99 = poc_latency_percentile(l, scratch, 99);

        off += snprintf(body + off, sizeof(body) - off,
            "poc_latency,role=%s,host=%s,stage=%s "
            "min=%luu,max=%luu,avg=%luu,p99=%luu,samples=%luu"
            "\n",
            role, s_host_tag, stages[i].name,
            (unsigned long)(l->min_us == UINT64_MAX ? 0 : l->min_us),
            (unsigned long)l->max_us,
            (unsigned long)poc_latency_avg(l),
            (unsigned long)p99,
            (unsigned long)l->count);
    }

    /* POST to InfluxDB */
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: text/plain; charset=utf-8");
    char auth_hdr[600];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: %s", s_auth_header);
    hdrs = curl_slist_append(hdrs, auth_hdr);

    curl_easy_setopt(s_curl, CURLOPT_URL, s_url);
    curl_easy_setopt(s_curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(s_curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(s_curl, CURLOPT_POSTFIELDSIZE, (long)off);
    curl_easy_setopt(s_curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(s_curl, CURLOPT_TIMEOUT_MS, 500L);        /* don't block pipeline */
    curl_easy_setopt(s_curl, CURLOPT_CONNECTTIMEOUT_MS, 200L);

    CURLcode res = curl_easy_perform(s_curl);
    if (res != CURLE_OK) {
        /* Log once in a while, don't spam */
        static unsigned s_err_cnt = 0;
        if (++s_err_cnt <= 3 || (s_err_cnt % 100 == 0))
            fprintf(stderr, "[influxdb] POST failed (%u): %s\n",
                    s_err_cnt, curl_easy_strerror(res));
    }

    curl_slist_free_all(hdrs);
}

void poc_influxdb_cleanup(void)
{
    if (s_curl) {
        curl_easy_cleanup(s_curl);
        s_curl = NULL;
    }
    curl_global_cleanup();
    s_enabled = false;
}
