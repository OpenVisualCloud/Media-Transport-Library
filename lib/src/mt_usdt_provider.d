/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * The USDT provider and probe define.
 */

provider sys {
  /* attach to enable the usdt log msg at runtime */
  probe log_msg(int level, char* msg);
  /* attach to enable the tasklet_time_measure at runtime */
  probe tasklet_time_measure();
  /* attach to enable the sessions_time_measure at runtime */
  probe sessions_time_measure();
  /* attach to enable the pcap dump for cni rx queue */
  probe cni_pcap_dump(int port, char* dump_file, uint32_t pkts);
}

provider ptp {
  probe ptp_msg(int port, int stage, uint64_t value);
  probe ptp_result(int port, int64_t delta, int64_t correct);
}

provider st20 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int m_idx, int s_idx, char* dump_file, void* va, uint32_t data_size);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
  probe rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int m_idx, int s_idx, char* dump_file, void* va, uint32_t data_size);
  /* attach to enable the pcap dump at runtime */
  probe rx_pcap_dump(int m_idx, int s_idx, int s_port, char* dump_file, uint32_t pkts);
  /* incomplete frame */
  probe rx_frame_incomplete(int m_idx, int s_idx, int f_idx, uint32_t tmstamp, uint32_t data_size, uint32_t expect_size);
}

provider st30 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int m_idx, int s_idx, char* dump_file, int frames);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
  probe rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int m_idx, int s_idx, char* dump_file, uint32_t data_size);
  /* attach to enable the pcap dump at runtime */
  probe rx_pcap_dump(int m_idx, int s_idx, int s_port, char* dump_file, uint32_t pkts);
}

provider st40 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t meta_num, int total_udw);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* rx */
  probe rx_mbuf_available(int m_idx, int s_idx, void* mbuf, uint32_t tmstamp, uint32_t data_size);
  probe rx_mbuf_enqueue_fail(int m_idx, int s_idx, void* mbuf, uint32_t tmstamp);
  probe rx_mbuf_put(int m_idx, int s_idx, void* mbuf);
}

provider st41 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t meta_num, int total_udw);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* rx */
  probe rx_mbuf_available(int m_idx, int s_idx, void* mbuf, uint32_t tmstamp, uint32_t data_size);
  probe rx_mbuf_enqueue_fail(int m_idx, int s_idx, void* mbuf, uint32_t tmstamp);
  probe rx_mbuf_put(int m_idx, int s_idx, void* mbuf);
}

provider st20p {
  /* tx */
  probe tx_frame_get(int idx, int f_idx, void* va);
  probe tx_frame_put(int idx, int f_idx, void* va, int stat);
  probe tx_frame_next(int idx, int f_idx);
  probe tx_frame_done(int idx, int f_idx, uint32_t tmstamp);
  probe tx_frame_drop(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int idx, char* dump_file, void* va, uint32_t data_size);
  /* rx */
  probe rx_frame_get(int idx, int f_idx, void* va);
  probe rx_frame_put(int idx, int f_idx, void* va);
  probe rx_frame_available(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int idx, char* dump_file, uint32_t data_size);
}

provider st22 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t codestream_size);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int m_idx, int s_idx, char* dump_file, void* va, uint32_t data_size);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
  probe rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int m_idx, int s_idx, char* dump_file, void* va, uint32_t data_size);
}

provider st22p {
  /* tx */
  probe tx_frame_get(int idx, int f_idx, void* va);
  probe tx_frame_put(int idx, int f_idx, void* va, int stat, uint32_t data_size);
  probe tx_frame_next(int idx, int f_idx);
  probe tx_frame_done(int idx, int f_idx, uint32_t tmstamp);
  probe tx_frame_drop(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int idx, char* dump_file, void* va, uint32_t data_size);
  /* rx */
  probe rx_frame_get(int idx, int f_idx, void* va, uint32_t data_size);
  probe rx_frame_put(int idx, int f_idx, void* va);
  probe rx_frame_available(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int idx, char* dump_file, uint32_t data_size);
  /* tx encode */
  probe tx_encode_get(int idx, int f_idx, void* src, void* dst);
  probe tx_encode_put(int idx, int f_idx, void* src, void* dst, int result, uint32_t data_size);
  /* rx decode */
  probe rx_decode_get(int idx, int f_idx, void* src, void* dst, uint32_t data_size);
  probe rx_decode_put(int idx, int f_idx, void* src, void* dst, int result);
}

provider st30p {
  /* tx */
  probe tx_frame_get(int idx, int f_idx, void* va);
  probe tx_frame_put(int idx, int f_idx, void* va);
  probe tx_frame_next(int idx, int f_idx);
  probe tx_frame_done(int idx, int f_idx, uint32_t tmstamp);
  probe tx_frame_drop(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe tx_frame_dump(int idx, char* dump_file, int frames);
  /* rx */
  probe rx_frame_get(int idx, int f_idx, void* va);
  probe rx_frame_put(int idx, int f_idx, void* va);
  probe rx_frame_available(int idx, int f_idx, uint32_t tmstamp);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int idx, char* dump_file, int frames);
}

provider st40p {
  /* tx */
  probe tx_frame_get(int idx, int f_idx, void* va);
  probe tx_frame_put(int idx, int f_idx, void* va);
  probe tx_frame_next(int idx, int f_idx);
  probe tx_frame_done(int idx, int f_idx, uint32_t tmstamp);
  probe tx_frame_drop(int idx, int f_idx, uint32_t tmstamp)
  /* rx */
  probe rx_frame_get(int idx, int f_idx, uint32_t meta_num);
  probe rx_frame_put(int idx, int f_idx, uint32_t meta_num);
  probe rx_frame_available(int idx, int f_idx, uint32_t meta_num);
  /* attach to enable the frame dump at runtime */
  probe rx_frame_dump(int idx, char* dump_file, uint32_t meta_num, uint32_t bytes);
}
