/* ex12 — RTP passthrough: the application builds the RTP packets, MTL adds
   UDP/IP/Ethernet, paces them on the ST 2110-21 schedule, and sends every packet on both
   ST 2022-7 legs. The same verbs as frames; a unit is a chunk of packet slots. */
#include <mtl/experimental/mtl_packet.h>

#include "ex_common.h"

#define PKTS_PER_FRAME 4320 /* 1080p 4:2:2 10-bit, 1200-byte payloads */
uint16_t packetise(uint8_t* slot, int64_t frame, uint32_t pkt); /* returns the length */
void inspect(const uint8_t* rtp, uint16_t len, uint8_t leg, uint32_t seq, uint16_t gap);

int open_packet_tx(mtl_instance_h mt, mtl_session_h* s) {
  struct mtl_session_config sc;
  MTL_INIT(&sc);
  sc.direction = MTL_TX;
  sc.essence = MTL_VIDEO;
  sc.unit = MTL_UNIT_PACKETS;
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 20, 20000);
  mtl_flow_ipv4(&sc.flows[1], 239, 168, 86, 20, 20000);
  sc.video.raster.width = 1920;
  sc.video.raster.height = 1080;
  sc.video.raster.rate = MTL_FPS_59_94;
  sc.video.format = MTL_YUV422_10;
  sc.packet.packets_per_unit = PKTS_PER_FRAME;
  sc.packet.set_fields =
      MTL_PKT_SET_TIMESTAMP | MTL_PKT_SET_SSRC_PT; /* the rest is ours */
  return mtl_session_create(mt, &sc, s);
}

int send_frame(mtl_session_h s, int64_t frame) {
  struct mtl_unit u;
  MTL_INIT(&u);
  for (uint32_t pkt = 0; pkt < PKTS_PER_FRAME;) {
    int ret = mtl_tx_acquire(s, &u, MTL_MS(20)); /* a chunk of packet slots */
    if (ret < 0) return ret;
    struct mtl_pkt_tx* len = mtl_pkt_tx_table(&u);
    uint32_t n = 0;
    for (; n < u.plane[0].rows && pkt < PKTS_PER_FRAME; n++, pkt++)
      len[n].len = packetise(mtl_pkt_slot(&u, n), frame, pkt);
    u.used = n;
    if (pkt == PKTS_PER_FRAME) u.flags |= MTL_SUBMIT_UNIT_END; /* the frame ends here */
    ret = mtl_tx_submit(s, &u); /* one result per chunk, if results are on */
    if (ret < 0) return ret;
  }
  return 0;
}

/* RX of a session with unit = MTL_UNIT_PACKETS: chunks of received packets, duplicates of
   the two legs already removed by sequence number. */
int receive_packets(mtl_session_h s) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_rx_dequeue(s, &u, MTL_MS(10));
  if (ret < 0) return ret;
  const struct mtl_pkt_rx* p = mtl_pkt_rx_table(&u);
  for (uint32_t i = 0; i < u.used; i++)
    inspect(p[i].data, p[i].len, p[i].leg, p[i].seq, p[i].gap);
  return mtl_rx_release(s, u.lease);
}
