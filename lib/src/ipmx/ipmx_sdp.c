/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "ipmx_sdp.h"

#include <stdarg.h>

#include "../mt_log.h"
#include "../mt_util.h"
#include "../st2110/st_tx_video_session.h"
#include "ipmx_sr.h"

/* the IPv4 TTL tv_init_hdr() sends with, ipv4->time_to_live in st_tx_video_session.c */
#define IPMX_SDP_TTL (64)

struct ipmx_sdp_out {
  char* buf;
  size_t len;
  size_t pos;
  bool full;
};

static void ipmx_sdp_add(struct ipmx_sdp_out* o, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void ipmx_sdp_add(struct ipmx_sdp_out* o, const char* fmt, ...) {
  va_list ap;
  int n;

  if (o->full) return;
  va_start(ap, fmt);
  n = vsnprintf(o->buf + o->pos, o->len - o->pos, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= o->len - o->pos) {
    o->full = true;
    return;
  }
  o->pos += n;
}

/* ST 2110-20 section 7, TR-10-1 sections 10.1 and 10.2 */
static void ipmx_sdp_add_fmtp(struct ipmx_sdp_out* o, const struct ipmx_sdp_media* m) {
  const struct ipmx_video_info* info = &m->info;

  ipmx_sdp_add(o, "a=fmtp:%u sampling=%.*s; width=%u; height=%u; ", m->payload_type,
               (int)sizeof(info->sampling), info->sampling, info->width, info->height);
  if (info->rate_den == 1)
    ipmx_sdp_add(o, "exactframerate=%u; ", info->rate_num);
  else
    ipmx_sdp_add(o, "exactframerate=%u/%u; ", info->rate_num, info->rate_den);
  ipmx_sdp_add(o,
               "depth=%u; TCS=%.*s; colorimetry=%.*s; RANGE=%.*s; PM=%s; "
               "SSN=ST2110-20:2017; TP=%s; IPMX; measuredpixclk=%" PRIu64
               "; vtotal=%u; htotal=%u\r\n",
               info->depth, (int)sizeof(info->tcs), info->tcs,
               (int)sizeof(info->colorimetry), info->colorimetry,
               (int)sizeof(info->range), info->range, info->gpm ? "2110GPM" : "2110BPM",
               m->wide ? "2110TPW" : "2110TPN", info->pixclk, info->vtotal, info->htotal);
}

static void ipmx_sdp_add_leg(struct ipmx_sdp_out* o, const struct ipmx_sdp_media* m,
                             int i) {
  static const char* const mid[MTL_SESSION_PORT_MAX] = {"primary", "secondary"};
  const struct ipmx_sdp_leg* leg = &m->leg[i];
  const uint8_t* d = leg->dip;
  const uint8_t* s = leg->sip;

  ipmx_sdp_add(o, "m=video %u RTP/AVP %u\r\n", leg->udp_port, m->payload_type);
  ipmx_sdp_add(o, "c=IN IP4 %u.%u.%u.%u", d[0], d[1], d[2], d[3]);
  /* RFC 8866 section 5.7: the TTL is for IPv4 multicast only */
  if (mt_is_multicast_ip(d)) ipmx_sdp_add(o, "/%u", IPMX_SDP_TTL);
  ipmx_sdp_add(o,
               "\r\na=source-filter: incl IN IP4 %u.%u.%u.%u %u.%u.%u.%u\r\n"
               "a=rtpmap:%u raw/90000\r\n",
               d[0], d[1], d[2], d[3], s[0], s[1], s[2], s[3], m->payload_type);
  ipmx_sdp_add_fmtp(o, m);
  ipmx_sdp_add(o, "a=ts-refclk:%.*s\r\na=mediaclk:%.*s\r\n",
               (int)sizeof(m->info.ts_refclk), m->info.ts_refclk,
               (int)sizeof(m->info.mediaclk), m->info.mediaclk);
  if (m->num_port > 1) ipmx_sdp_add(o, "a=mid:%s\r\n", mid[i]);
}

int ipmx_sdp_build(const struct ipmx_sdp_media* m, uint64_t sess_id,
                   uint64_t sess_version, char* buf, size_t len) {
  struct ipmx_sdp_out o = {.buf = buf, .len = len};
  const uint8_t* s = m->leg[MTL_SESSION_PORT_P].sip;
  char name[IPMX_SDP_NAME_MAX];

  /* the fmtp has no interlace, segmented or PAR parameter yet */
  if (m->info.interlaced || m->info.segmented || m->info.par_w != m->info.par_h)
    return -ENOTSUP;

  /* RFC 8866 section 5.3: one non-empty line, ASCII for parsers that decode UTF-8 */
  snprintf(name, sizeof(name), "%.*s", (int)sizeof(m->name) - 1,
           m->name[0] ? m->name : "-");
  for (char* c = name; *c; c++)
    if ((unsigned char)*c < ' ' || (unsigned char)*c >= 0x7f) *c = '_';

  ipmx_sdp_add(&o, "v=0\r\no=- %" PRIu64 " %" PRIu64 " IN IP4 %u.%u.%u.%u\r\n", sess_id,
               sess_version, s[0], s[1], s[2], s[3]);
  ipmx_sdp_add(&o, "s=%s\r\nt=0 0\r\n", name);
  /* ST 2110-10 section 8.3, RFC 7104 */
  if (m->num_port > 1) ipmx_sdp_add(&o, "a=group:DUP primary secondary\r\n");
  for (int i = 0; i < m->num_port; i++) ipmx_sdp_add_leg(&o, m, i);

  if (o.full) {
    if (len) buf[0] = '\0';
    return -ENOSPC;
  }
  return o.pos;
}

/* the wire state of s, under the session lock that serializes destination updates */
static void ipmx_sdp_media_of(struct st_tx_video_session_impl* s,
                              struct ipmx_sdp_media* m) {
  RTE_BUILD_BUG_ON(IPMX_SDP_NAME_MAX != ST_MAX_NAME_LEN);
  m->payload_type = s->s_hdr[MTL_SESSION_PORT_P].rtp.base.payload_type;
  m->wide = s->ops.pacing == ST21_PACING_WIDE;
  m->num_port = s->ipmx_sr->num_port;
  snprintf(m->name, sizeof(m->name), "%s", s->ops_name);
  for (int i = 0; i < m->num_port; i++) {
    const struct st_rfc4175_video_hdr* hdr = &s->s_hdr[i];

    memcpy(m->leg[i].sip, &hdr->ipv4.src_addr, MTL_IP_ADDR_LEN);
    memcpy(m->leg[i].dip, &hdr->ipv4.dst_addr, MTL_IP_ADDR_LEN);
    m->leg[i].udp_port = ntohs(hdr->udp.dst_port);
  }
}

int ipmx_sdp_tx_get(struct st_tx_video_session_impl* s, char* buf, size_t len) {
  struct ipmx_sr_tx* sr = s->ipmx_sr;
  rte_spinlock_t* lock = &s->mgr->mutex[s->idx];
  struct ipmx_sdp_media m;
  uint64_t version;
  int ret;

  if (!sr) {
    err("%s(%d), IPMX not enabled\n", __func__, s->idx);
    return -EIO;
  }
  if (!buf || !len) {
    err("%s(%d), NULL buf or zero len\n", __func__, s->idx);
    return -EINVAL;
  }

  memset(&m, 0, sizeof(m));
  /* snapshot and version under one lock, so a later version never has older content */
  rte_spinlock_lock(lock);
  ret = ipmx_sr_tx_get_info(s, &m.info);
  if (ret < 0) {
    rte_spinlock_unlock(lock);
    return ret;
  }
  ipmx_sdp_media_of(s, &m);
  if (sr->sdp_taken && memcmp(&m, &sr->sdp_last, sizeof(m))) sr->sdp_sess_version++;
  memcpy(&sr->sdp_last, &m, sizeof(m));
  sr->sdp_taken = true;
  version = sr->sdp_sess_version;
  rte_spinlock_unlock(lock);

  return ipmx_sdp_build(&m, sr->sdp_sess_id, version, buf, len);
}
