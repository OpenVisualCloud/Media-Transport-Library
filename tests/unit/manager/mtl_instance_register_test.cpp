/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins how MtlManager answers a register message, read from the client end of a
 * socketpair. This build has no XDP backend and ifindex 0 never exists, so every lookup
 * answers -1.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtlManagerRegisterTest.*'
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <memory>

#include "../../../manager/mtl_instance.hpp"

class MtlManagerRegisterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    instance_ = std::make_unique<mtl_instance>(fds[0]);
    client_fd_ = fds[1];
  }

  void TearDown() override {
    instance_.reset();
    close(client_fd_);
  }

  /* The one response to a register message naming num_if interfaces. */
  int register_response(uint16_t num_if) {
    mtl_message_t msg = {};
    msg.header.magic = htonl(MTL_MANAGER_MAGIC);
    msg.header.type = (mtl_message_type_t)htonl(MTL_MSG_TYPE_REGISTER);
    msg.header.body_len = htonl(sizeof(mtl_register_message_t));
    /* the manager logs all 64 bytes, and a NUL makes the saved log binary */
    memset(msg.body.register_msg.hostname, 'u', sizeof(msg.body.register_msg.hostname));
    msg.body.register_msg.num_if = htons(num_if);
    instance_->handle_message(reinterpret_cast<const char*>(&msg), sizeof(msg));

    mtl_message_t resp = {}, more;
    EXPECT_EQ(recv(client_fd_, &resp, sizeof(resp), MSG_DONTWAIT), (ssize_t)sizeof(resp));
    EXPECT_EQ(ntohl(resp.header.type), (uint32_t)MTL_MSG_TYPE_RESPONSE);
    EXPECT_EQ(recv(client_fd_, &more, sizeof(more), MSG_DONTWAIT), -1);
    return (int)ntohl(resp.body.response_msg.response);
  }

  std::unique_ptr<mtl_instance> instance_;
  int client_fd_ = -1;
};

TEST_F(MtlManagerRegisterTest, NumIfPastTheIfindexArrayIsRejected) {
  EXPECT_EQ(register_response(9), -EINVAL);
  EXPECT_EQ(register_response(UINT16_MAX), -EINVAL);
}

TEST_F(MtlManagerRegisterTest, NumIfOfTheWholeArrayReachesTheLookup) {
  EXPECT_EQ(register_response(8), -1);
}

TEST_F(MtlManagerRegisterTest, NoInterfaceRegisters) {
  EXPECT_EQ(register_response(0), 0);
}
