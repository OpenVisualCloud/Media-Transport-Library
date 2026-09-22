/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20_LEN_USER_META copy bounds in rv_handle_frame_pkt. The meta length comes
 * from the wire row_length field and the copy that consumes it runs before any
 * pkt-length check, bounded only against the application's buffer, so a pkt may
 * declare up to user_meta_buffer_size bytes while carrying almost none;
 * mt_memcpy then hands the application whatever a previous pkt left in the
 * mbuf's data room (CWE-125).
 *
 * ut20_ctx_enable_user_meta() in SetUp() is what makes this evidence: with the
 * default frames user_meta_buffer_size is 0, every user-meta pkt is already
 * refused for lack of room, and the assertions would pass unfixed.
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

/* sizeof(struct st_rfc4175_video_hdr): eth + ip + udp + rtp, the bytes before
 * the meta payload in a pkt with no extra RTP header. */
static constexpr uint16_t kHdrLen = 62;

class St20RxUserMetaTest : public St20RxBaseTest {
 protected:
  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_user_meta(ctx_);
  }

  uint64_t user_meta() const {
    return ut20_stat_user_meta(ctx_);
  }

  uint64_t user_meta_err() const {
    return ut20_stat_user_meta_err(ctx_);
  }
};

/* Both ways the declared length can exceed the bytes present. First: 1000 bytes
 * of meta declared on a datagram carrying 8 -- unfixed, 1000 <= 1024 passes the
 * only check and mt_memcpy reads ~992 bytes past the datagram, then reports
 * success. Second: a datagram shorter than the header itself, as the kernel
 * socket path can deliver, declaring 8 bytes -- well within the app buffer, so
 * only a bound against the bytes actually present rejects it. */
TEST_F(St20RxUserMetaTest, UserMetaLongerThanPktRejected) {
  EXPECT_LT(
      ut20_feed_user_meta_pkt(ctx_, 100, 1000, 1000, kHdrLen + 8, MTL_SESSION_PORT_P), 0);
  EXPECT_LT(ut20_feed_user_meta_pkt(ctx_, 200, 2000, 8, 42, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(user_meta_err(), 2u);
  EXPECT_EQ(user_meta(), 0u);
}

/* A well-formed user-meta pkt whose declared length is exactly the payload it
 * carries must still be accepted, so the new bound cannot be off by one in the
 * rejecting direction -- a `<` instead of `<=` fails here. The pkt is meta-only,
 * so it is not counted as a received video pkt. */
TEST_F(St20RxUserMetaTest, LegitimateUserMetaAccepted) {
  EXPECT_EQ(
      ut20_feed_user_meta_pkt(ctx_, 100, 1000, 16, kHdrLen + 16, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(user_meta(), 1u);
  EXPECT_EQ(user_meta_err(), 0u);
  EXPECT_EQ(received(), 0u);
}
