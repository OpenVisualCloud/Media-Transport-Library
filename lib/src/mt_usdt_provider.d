/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * The USDT provider and probe define.
 */

provider sys {
  probe log_msg(int level, char* msg);
}

provider ptp {
  probe ptp_msg(int port, int stage, uint64_t value);
  probe ptp_result(int port, int64_t delta, int64_t correct);
}

provider st20 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
  probe rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp);
}

provider st30 {
  /* tx */
  probe tx_frame_next(int m_idx, int s_idx, int f_idx, void* va);
  probe tx_frame_done(int m_idx, int s_idx, int f_idx, uint32_t tmstamp);
  /* rx */
  probe rx_frame_available(int m_idx, int s_idx, int f_idx, void* va, uint32_t tmstamp, uint32_t data_size);
  probe rx_frame_put(int m_idx, int s_idx, int f_idx, void* va);
  probe rx_no_framebuffer(int m_idx, int s_idx, uint32_t tmstamp);
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
