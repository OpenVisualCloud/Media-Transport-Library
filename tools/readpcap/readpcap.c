/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <pcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_S 1000000000
#define TR_OFFSET_NS (500 * 1000) // 500us

static inline uint64_t TimespecToNs(const struct timeval *ts) {
  return ((uint64_t)ts->tv_sec * NS_PER_S) + ts->tv_usec;
}

static int g_pkt_idx = 0;
static int g_frame_idx = -1;
static uint64_t last_tn = 0;
static uint64_t frame_tn = 0;
static int g_target_frame_idx = -2;

void packetHandler(u_char *userData, const struct pcap_pkthdr *pkthdr,
                   const u_char *packet) {
  uint64_t tn = TimespecToNs(&pkthdr->ts);
  if (last_tn) {
    if ((tn - last_tn) > TR_OFFSET_NS) {
      g_frame_idx++;
      frame_tn = tn;
      // printf("Find a new frame %d at pkt %d\n", g_frame_idx, g_pkt_idx);
    } else if (g_target_frame_idx == g_frame_idx) {
      // printf("%"PRIu64"\n", tn - frame_tn);
      printf("%" PRIu64 "\n", tn - last_tn);
    }
  }

  g_pkt_idx++;
  last_tn = tn;
}

int main(int argc, char **argv) {
  pcap_t *fp;
  char errbuf[PCAP_ERRBUF_SIZE];
  char source[1500];
  int i, maxCountSyn = 0, maxCountHttp = 0, maxIdxSyn = 0, maxIdxHttp = 0;

  if (argc < 2) {
    printf("usage: %s filename index\n", argv[0]);
    return -1;
  }

  if (argc > 2) {
    g_target_frame_idx = atoi(argv[2]);
    printf("target_frame %d\n", g_target_frame_idx);
  }

  fp = pcap_open_offline_with_tstamp_precision(
      argv[1], PCAP_TSTAMP_PRECISION_NANO, errbuf);
  if (fp == NULL) {
    fprintf(stderr, "pcap_open_offline() failed: %s\n", errbuf);
    return 0;
  }

  if (pcap_loop(fp, -1, packetHandler, NULL) < 0) {
    fprintf(stderr, "pcap_loop() failed: %s\n", pcap_geterr(fp));
    pcap_close(fp);
    return 0;
  }

  pcap_close(fp);

  printf("Total frame %d, total pkt %d\n", g_frame_idx, g_pkt_idx);
}
