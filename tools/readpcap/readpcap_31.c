/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <pcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define RFC3550_RTP_HDR_LEN (12)

#define ST_LITTLE_ENDIAN

/* The 32-bit audio data */
struct am824_be {
#ifdef ST_LITTLE_ENDIAN
  uint8_t v : 1;
  uint8_t u : 1;
  uint8_t c : 1;
  uint8_t p : 1;
  uint8_t f : 1;
  uint8_t b : 1;
  uint8_t unused : 2;
#else
  uint8_t unused : 2;
  uint8_t b : 1;
  uint8_t f : 1;
  uint8_t p : 1;
  uint8_t c : 1;
  uint8_t u : 1;
  uint8_t v : 1;
#endif
  uint8_t data[3];
} __attribute__((__packed__));
typedef struct am824_be am824_t;

struct aes3_le {
#ifdef ST_LITTLE_ENDIAN
  uint8_t preamble : 4;
  uint8_t data_0 : 4;
  uint16_t data_1;
  uint8_t data_2 : 4;
  uint8_t v : 1;
  uint8_t u : 1;
  uint8_t c : 1;
  uint8_t p : 1;
#else
  uint8_t data_0 : 4;
  uint8_t preamble : 4;
  uint16_t data_1;
  uint8_t p : 1;
  uint8_t c : 1;
  uint8_t u : 1;
  uint8_t v : 1;
  uint8_t data_2 : 4;
#endif
} __attribute__((__packed__));
typedef struct aes3_le aes3_t;

struct user_data {
  uint8_t* cursor;
  uint8_t* begin;
  uint8_t* end;
};

static int g_pkt_idx = 0;

void packetHandler(uint8_t* userData, const struct pcap_pkthdr* pkthdr,
                   const uint8_t* packet) {
  struct user_data* ud = (struct user_data*)userData;
  uint8_t* payload = packet + sizeof(struct ether_header) + sizeof(struct iphdr) +
                     sizeof(struct udphdr) + RFC3550_RTP_HDR_LEN;
  uint16_t payload_len = pkthdr->len - sizeof(struct ether_header) -
                         sizeof(struct iphdr) - sizeof(struct udphdr) -
                         RFC3550_RTP_HDR_LEN;
  if (payload_len % 4 != 0) {
    printf("wrong am824 packet! payload_len %u\n", payload_len);
    return;
  }
  int num_subframes = payload_len / 4;
  int sample_per_packet = 48;  // 48khz, 1ms packet time
  int num_channels = num_subframes / sample_per_packet;
  printf("pkt %d, %d subframes of %d channels\n", g_pkt_idx, num_subframes, num_channels);
  am824_t* am = (am824_t*)payload;
  for (int i = 0; i < num_subframes; i++) {
    uint32_t am_32 = *(uint32_t*)am;
    printf("pkt %d, subframe %d, hex: %08x, channel bit: %u\n", g_pkt_idx, i, am_32,
           am->c);
    if (am->f) {
      printf("pkt %d, subframe %d, first subframe of frame\n", g_pkt_idx, i);
      if (am->b) {
        printf("pkt %d, subframe %d, first subframe of block %08x\n", g_pkt_idx, i,
               am_32);
      }
    }

    if (am->unused) {
      printf("pkt %d, subframe %d, unused bit not zero!\n", g_pkt_idx, i);
    }

    if (ud->cursor < ud->end) {
      memcpy(ud->cursor, am, 4);
      ud->cursor += 4;
    }
    am++;
  }
  g_pkt_idx++;
}

int main(int argc, char** argv) {
  pcap_t* fp;
  int fd;
  char errbuf[PCAP_ERRBUF_SIZE];

  if (argc < 2) {
    printf("usage: %s filename index\n", argv[0]);
    return -1;
  }

  fd = open("out.824", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    printf("output file open fail\n");
    return -1;
  }

  int f_size = 192 * 2 * 4 * 8;
  int ret = ftruncate(fd, f_size);
  if (ret < 0) {
    printf("ftruncate fail\n");
    close(fd);
    return -1;
  }

  struct user_data ud;

  uint8_t* m = mmap(NULL, f_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    printf("mmap fail\n");
    close(fd);
    return -1;
  }

  ud.cursor = m;
  ud.begin = m;
  ud.end = m + f_size;

  fp = pcap_open_offline(argv[1], errbuf);
  if (fp == NULL) {
    fprintf(stderr, "pcap_open_offline() failed: %s\n", errbuf);
    close(fd);
    return 0;
  }

  if (pcap_loop(fp, -1, packetHandler, &ud) < 0) {
    fprintf(stderr, "pcap_loop() failed: %s\n", pcap_geterr(fp));
    close(fd);
    pcap_close(fp);
    return 0;
  }

  munmap(ud.begin, ud.end - ud.begin);
  close(fd);
  pcap_close(fp);

  printf("total pkt %d\n", g_pkt_idx);
}
