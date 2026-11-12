/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins how MtlManager answers a register message, read from the client end of a
 * socketpair. This build has no XDP backend and ifindex 0 never exists, so every lookup
 * answers -1.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtlManagerRegister*'
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <memory>
#include <vector>

#include "../../../manager/mtl_instance.hpp"

static mtl_message_t register_message(uint16_t num_if) {
  mtl_message_t msg = {};
  msg.header.magic = htonl(MTL_MANAGER_MAGIC);
  msg.header.type = (mtl_message_type_t)htonl(MTL_MSG_TYPE_REGISTER);
  msg.header.body_len = htonl(sizeof(mtl_register_message_t));
  /* the manager logs all 64 bytes, and a NUL makes the saved log binary */
  memset(msg.body.register_msg.hostname, 'u', sizeof(msg.body.register_msg.hostname));
  msg.body.register_msg.num_if = htons(num_if);
  return msg;
}

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

  void feed(const mtl_message_t& msg) {
    instance_->handle_message(reinterpret_cast<const char*>(&msg), sizeof(msg));
  }

  /* The one response to a register message naming num_if interfaces. */
  int register_response(uint16_t num_if) {
    feed(register_message(num_if));

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

TEST_F(MtlManagerRegisterTest, ReplyIsZeroPastTheResponse) {
  mtl_message_t want = {}, got;
  want.header.magic = htonl(MTL_MANAGER_MAGIC);
  want.header.type = (mtl_message_type_t)htonl(MTL_MSG_TYPE_RESPONSE);
  want.header.body_len = htonl(sizeof(mtl_response_message_t));

  feed(register_message(0));
  ASSERT_EQ(recv(client_fd_, &got, sizeof(got), MSG_DONTWAIT), (ssize_t)sizeof(got));
  const auto* g = reinterpret_cast<const uint8_t*>(&got);
  const auto* w = reinterpret_cast<const uint8_t*>(&want);
  EXPECT_EQ(std::vector<uint8_t>(g, g + sizeof(got)),
            std::vector<uint8_t>(w, w + sizeof(want)));
}

namespace {

volatile sig_atomic_t g_sigpipe_cnt;

void count_sigpipe(int) {
  g_sigpipe_cnt = g_sigpipe_cnt + 1;
}

}  // namespace

/* SIGPIPE is unblocked and caught, so a raised one is counted, not hidden or fatal. */
class MtlManagerRegisterSigpipeTest : public MtlManagerRegisterTest {
 protected:
  void SetUp() override {
    MtlManagerRegisterTest::SetUp();
    sigset_t sigpipe;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    ASSERT_EQ(pthread_sigmask(SIG_UNBLOCK, &sigpipe, &old_mask_), 0);
    struct sigaction sa = {};
    sa.sa_handler = count_sigpipe;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(sigaction(SIGPIPE, &sa, &old_sa_), 0);
    g_sigpipe_cnt = 0;
  }

  void TearDown() override {
    sigaction(SIGPIPE, &old_sa_, nullptr);
    pthread_sigmask(SIG_SETMASK, &old_mask_, nullptr);
    MtlManagerRegisterTest::TearDown();
  }

  sigset_t old_mask_ = {};
  struct sigaction old_sa_ = {};
};

TEST_F(MtlManagerRegisterSigpipeTest, AnswerToAClosedClientRaisesNone) {
  close(client_fd_);
  client_fd_ = -1;
  feed(register_message(0));
  EXPECT_EQ(g_sigpipe_cnt, 0);
}
