/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * What the client library puts on the wire.
 *
 * The other test files drive mtl_instance from a record the test built, so they
 * prove the server side of every field. These cases prove the other side: each
 * function of mtlm_api.h against the bytes it writes and the answer it reads.
 * A swap that the client and the manager both got wrong the same way would pass
 * a round trip and fail against any other implementation, so the record is read
 * here field by field.
 *
 * The test binds a socket of its own and answers by hand. No manager runs.
 */

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "mtlm_api.h"
#include "mtlm_test_wire.hpp"

namespace {

/** A socket file in the test directory that nothing listens on. */
constexpr const char* kStaleName = "/stale.sock";

class ClientWireTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mtlm-client-wire-XXXXXX";

    ASSERT_NE(mkdtemp(pattern), nullptr);
    dir = pattern;
    path = dir + "/mtl_manager.sock";

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    struct sockaddr_un addr = unix_addr(path);
    ASSERT_EQ(bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0)
        << std::strerror(errno);
    ASSERT_EQ(listen(listen_fd, 1), 0);

    client = mtlm_client_create(path.c_str());
    ASSERT_NE(client, nullptr) << std::strerror(errno);

    conn_fd = accept(listen_fd, nullptr, nullptr);
    ASSERT_GE(conn_fd, 0);
    wire_set_timeout(conn_fd, 5);
  }

  void TearDown() override {
    if (client != nullptr) mtlm_client_destroy(client);
    if (conn_fd >= 0) close(conn_fd);
    if (listen_fd >= 0) close(listen_fd);
    unlink(path.c_str());
    unlink((dir + kStaleName).c_str());
    rmdir(dir.c_str());
  }

  /**
   * Run `call` in a thread, read the record it sent, and answer it.
   *
   * The client blocks until it has an answer, so the call and the answer cannot
   * be in the same thread. The record the client sent lands in `sent`.
   */
  int exchange(const std::function<int()>& call, mtl_message_t& sent, int response,
               mtl_message_type_t reply_type = MTL_MSG_TYPE_RESPONSE) {
    int result = 0;
    std::thread worker([&]() { result = call(); });

    int ret = wire_read(conn_fd, sent);
    if (ret == 0) {
      mtl_message_t reply = wire_request(reply_type, sizeof(mtl_response_message_t));

      reply.body.response_msg.response =
          static_cast<int>(htonl(static_cast<uint32_t>(response)));
      ret = static_cast<int>(send(conn_fd, &reply, sizeof(reply), 0)) == sizeof(reply)
                ? 0
                : -EIO;
    }

    worker.join();
    return ret == 0 ? result : ret;
  }

  int listen_fd = -1;
  int conn_fd = -1;
  mtlm_client* client = nullptr;
  std::string dir;
  std::string path;
};

} /* namespace */

/*
 * The manager gives back what an instance holds when its socket closes. A child
 * that inherits the socket over an exec keeps it open, so the lcores, queues and
 * flow rules of an instance that died stay taken for as long as the child lives.
 */
TEST_F(ClientWireTest, TheManagerSocketCannotOutliveAnExec) {
  int flags = fcntl(mtlm_client_fd(client), F_GETFD);

  ASSERT_GE(flags, 0) << std::strerror(errno);
  EXPECT_TRUE(flags & FD_CLOEXEC) << "the manager socket survives an exec";
}

TEST_F(ClientWireTest, EveryRecordCarriesTheMagicAndTheSize) {
  mtl_message_t sent;

  ASSERT_EQ(exchange([&]() { return mtlm_lcore_get(client, 3); }, sent, 0), 0);
  EXPECT_EQ(ntohl(sent.header.magic), static_cast<uint32_t>(MTL_MANAGER_MAGIC));
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_GET_LCORE));
  EXPECT_EQ(ntohl(sent.header.body_len), sizeof(mtl_lcore_message_t));
}

TEST_F(ClientWireTest, RegisterSendsTheFieldsItWasGiven) {
  unsigned int ifindex[3] = {7, 8, 9};
  struct mtlm_register_args args = {};
  mtl_message_t sent;

  args.pid = 4321;
  args.uid = 1000;
  args.hostname = "a-host";
  args.ifindex = ifindex;
  args.num_if = 3;

  ASSERT_EQ(exchange([&]() { return mtlm_register(client, &args); }, sent, 0), 0);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_REGISTER));
  EXPECT_EQ(ntohl(static_cast<uint32_t>(sent.body.register_msg.pid)), 4321u);
  EXPECT_EQ(ntohl(static_cast<uint32_t>(sent.body.register_msg.uid)), 1000u);
  EXPECT_STREQ(sent.body.register_msg.hostname, "a-host");
  EXPECT_EQ(ntohs(sent.body.register_msg.num_if), 3);
  for (int i = 0; i < 3; i++)
    EXPECT_EQ(ntohl(sent.body.register_msg.ifindex[i]), ifindex[i]) << "index " << i;
}

TEST_F(ClientWireTest, RegisterFillsInTheFieldsItWasNotGiven) {
  struct mtlm_register_args args = {};
  char hostname[MTL_MANAGER_HOSTNAME_LEN] = {};
  mtl_message_t sent;

  /* pid 0 and uid -1 ask the API for the real values, so a caller does not have
   * to repeat getpid() and getuid() itself, and cannot claim to be another
   * process by leaving a field at zero. */
  args.uid = -1;
  ASSERT_EQ(gethostname(hostname, sizeof(hostname) - 1), 0);

  ASSERT_EQ(exchange([&]() { return mtlm_register(client, &args); }, sent, 0), 0);
  EXPECT_EQ(ntohl(static_cast<uint32_t>(sent.body.register_msg.pid)),
            static_cast<uint32_t>(getpid()));
  EXPECT_EQ(ntohl(static_cast<uint32_t>(sent.body.register_msg.uid)),
            static_cast<uint32_t>(getuid()));
  EXPECT_STREQ(sent.body.register_msg.hostname, hostname);
  EXPECT_EQ(ntohs(sent.body.register_msg.num_if), 0);
}

TEST_F(ClientWireTest, TooManyInterfacesNeverReachTheWire) {
  unsigned int ifindex[MTL_MANAGER_MAX_IF + 1] = {};
  struct mtlm_register_args args = {};
  char byte = 0;

  args.uid = -1;
  args.ifindex = ifindex;
  args.num_if = MTL_MANAGER_MAX_IF + 1;

  /* The record has room for MTL_MANAGER_MAX_IF indexes. Sending more would ask
   * the manager to read past the end of it, so the API refuses without writing
   * anything. */
  EXPECT_EQ(mtlm_register(client, &args), -EINVAL);

  wire_set_timeout(conn_fd, 1);
  EXPECT_LT(recv(conn_fd, &byte, 1, 0), 1);
}

TEST_F(ClientWireTest, LcoreCallsCarryTheId) {
  mtl_message_t sent;

  ASSERT_EQ(exchange([&]() { return mtlm_lcore_get(client, 300); }, sent, 0), 0);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_GET_LCORE));
  EXPECT_EQ(ntohs(sent.body.lcore_msg.lcore), 300);

  ASSERT_EQ(exchange([&]() { return mtlm_lcore_put(client, 300); }, sent, 0), 0);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_PUT_LCORE));
  EXPECT_EQ(ntohs(sent.body.lcore_msg.lcore), 300);
}

TEST_F(ClientWireTest, QueueCallsCarryTheInterfaceAndTheQueue) {
  mtl_message_t sent;

  /* The manager answers a get with the queue id in the response field, so a
   * positive answer must come back as it is and not as a success code. */
  EXPECT_EQ(exchange([&]() { return mtlm_queue_get(client, 42); }, sent, 5,
                     MTL_MSG_TYPE_IF_QUEUE_ID),
            5);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_IF_GET_QUEUE));
  EXPECT_EQ(ntohl(sent.body.if_msg.ifindex), 42u);

  ASSERT_EQ(exchange([&]() { return mtlm_queue_put(client, 42, 5); }, sent, 0), 0);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_IF_PUT_QUEUE));
  EXPECT_EQ(ntohl(sent.body.if_msg.ifindex), 42u);
  EXPECT_EQ(ntohs(sent.body.if_msg.queue_id), 5);
}

TEST_F(ClientWireTest, FlowAddSendsAddressesInNetworkOrderAndPortsInHostOrder) {
  struct mtlm_flow flow = {};
  mtl_message_t sent;

  flow.ifindex = 42;
  flow.queue_id = 3;
  flow.flow_type = 0x02; /* UDP_V4_FLOW */
  flow.src_ip = 0x0100000a;
  flow.dst_ip = 0x0200000a;
  flow.src_port = 1234;
  flow.dst_port = 5678;

  EXPECT_EQ(exchange([&]() { return mtlm_flow_add(client, &flow); }, sent, 7,
                     MTL_MSG_TYPE_IF_FLOW_ID),
            7);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_IF_ADD_FLOW));
  EXPECT_EQ(ntohl(sent.body.if_msg.ifindex), 42u);
  EXPECT_EQ(ntohs(sent.body.if_msg.queue_id), 3);
  EXPECT_EQ(ntohl(sent.body.if_msg.flow_type), 0x02u);
  /* The caller hands over an address that is already network byte order, which
   * is the order ethtool wants, so the client must pass it through untouched.
   * Copy each one out first: the record is packed, and EXPECT_EQ binds a
   * reference to its argument, which a field of a packed struct cannot give. */
  uint32_t src_ip = sent.body.if_msg.src_ip;
  uint32_t dst_ip = sent.body.if_msg.dst_ip;
  EXPECT_EQ(src_ip, 0x0100000au);
  EXPECT_EQ(dst_ip, 0x0200000au);
  /* A port is a number to the caller, so it travels like every other number. */
  EXPECT_EQ(ntohs(sent.body.if_msg.src_port), 1234);
  EXPECT_EQ(ntohs(sent.body.if_msg.dst_port), 5678);
}

TEST_F(ClientWireTest, FlowDeleteCarriesTheIdItWasGiven) {
  mtl_message_t sent;

  ASSERT_EQ(exchange([&]() { return mtlm_flow_del(client, 42, 0x80000001u); }, sent, 0),
            0);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_IF_DEL_FLOW));
  EXPECT_EQ(ntohl(sent.body.if_msg.ifindex), 42u);
  EXPECT_EQ(ntohl(sent.body.if_msg.flow_id), 0x80000001u);
}

TEST_F(ClientWireTest, FilterCallsCarryTheInterfaceAndThePort) {
  mtl_message_t sent;

  ASSERT_EQ(
      exchange([&]() { return mtlm_udp_dp_filter_add(client, 42, 65535); }, sent, 0), 0);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_ADD_UDP_DP_FILTER));
  EXPECT_EQ(ntohl(sent.body.udp_dp_filter_msg.ifindex), 42u);
  EXPECT_EQ(ntohs(sent.body.udp_dp_filter_msg.port), 65535);

  ASSERT_EQ(
      exchange([&]() { return mtlm_udp_dp_filter_del(client, 42, 65535); }, sent, 0), 0);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_DEL_UDP_DP_FILTER));
  EXPECT_EQ(ntohl(sent.body.udp_dp_filter_msg.ifindex), 42u);
  EXPECT_EQ(ntohs(sent.body.udp_dp_filter_msg.port), 65535);
}

TEST_F(ClientWireTest, HeartbeatEchoesTheSequenceNumberAndNotAResponseCode) {
  mtl_message_t sent;
  uint32_t acked = 0;

  /* The ack carries the sequence number in the bytes a response carries its
   * code, so a number with the top bit set must not come back as an error. */
  wire_worker worker(
      conn_fd, [&]() { EXPECT_EQ(mtlm_heartbeat(client, 0xdeadbeefu, &acked), 0); });

  ASSERT_EQ(wire_read(conn_fd, sent), 0);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_HEARTBEAT));
  EXPECT_EQ(ntohl(sent.body.heartbeat_msg.seq), 0xdeadbeefu);

  mtl_message_t reply =
      wire_request(MTL_MSG_TYPE_HEARTBEAT_ACK, sizeof(mtl_heartbeat_message_t));
  reply.body.heartbeat_msg.seq = sent.body.heartbeat_msg.seq;
  ASSERT_EQ(send(conn_fd, &reply, sizeof(reply), 0), static_cast<ssize_t>(sizeof(reply)));

  worker.join();
  EXPECT_EQ(acked, 0xdeadbeefu);
}

TEST_F(ClientWireTest, AReplyOfTheWrongTypeIsRefused) {
  mtl_message_t sent;

  /* A manager that answers a lcore request with a queue id has a fault, and
   * reading the response field anyway would report a queue id as an errno. */
  EXPECT_EQ(exchange([&]() { return mtlm_lcore_get(client, 1); }, sent, 0,
                     MTL_MSG_TYPE_IF_QUEUE_ID),
            -EBADMSG);
}

TEST_F(ClientWireTest, AReplyWithTheWrongMagicIsRefused) {
  mtl_message_t sent;
  int result = 0;
  wire_worker worker(conn_fd, [&]() { result = mtlm_lcore_get(client, 1); });

  ASSERT_EQ(wire_read(conn_fd, sent), 0);

  mtl_message_t reply =
      wire_request(MTL_MSG_TYPE_RESPONSE, sizeof(mtl_response_message_t));
  reply.header.magic = htonl(0x0badf00d);
  ASSERT_EQ(send(conn_fd, &reply, sizeof(reply), 0), static_cast<ssize_t>(sizeof(reply)));

  worker.join();
  EXPECT_EQ(result, -EBADMSG);
}

TEST_F(ClientWireTest, AManagerThatClosesMidRecordIsReported) {
  mtl_message_t sent;
  int result = 0;
  wire_worker worker(conn_fd, [&]() { result = mtlm_lcore_get(client, 1); });

  ASSERT_EQ(wire_read(conn_fd, sent), 0);

  /* Half a record and then a close. The client must report it rather than treat
   * the unwritten half of its buffer as an answer. */
  mtl_message_t reply =
      wire_request(MTL_MSG_TYPE_RESPONSE, sizeof(mtl_response_message_t));
  ASSERT_GT(send(conn_fd, &reply, sizeof(reply) / 2, 0), 0);
  close(conn_fd);
  conn_fd = -1;

  worker.join();
  EXPECT_EQ(result, -ECONNRESET);
}

TEST_F(ClientWireTest, ARecordSplitByTheManagerIsPutBackTogether) {
  mtl_message_t sent;
  int result = 0;
  wire_worker worker(conn_fd, [&]() { result = mtlm_lcore_get(client, 1); });

  ASSERT_EQ(wire_read(conn_fd, sent), 0);

  /* A stream socket may move the answer in pieces. The client must wait for the
   * whole record. */
  mtl_message_t reply =
      wire_request(MTL_MSG_TYPE_RESPONSE, sizeof(mtl_response_message_t));
  reply.body.response_msg.response =
      static_cast<int>(htonl(static_cast<uint32_t>(-EBUSY)));
  const char* at = reinterpret_cast<const char*>(&reply);
  ASSERT_EQ(send(conn_fd, at, 3, 0), 3);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_EQ(send(conn_fd, at + 3, sizeof(reply) - 3, 0),
            static_cast<ssize_t>(sizeof(reply) - 3));

  worker.join();
  EXPECT_EQ(result, -EBUSY);
}

/*
 * The one message whose answer carries a descriptor.
 *
 * Nothing above covers it, because it does not use the exchange() helper: the
 * answer is one data byte plus control data, and not a record. The cases below
 * answer it by hand, and count the open descriptors of the process, because a
 * descriptor the client takes and does not give back is a leak no return value
 * reports. A manager runs for months, and an MTL instance asks for this map on
 * every port it opens.
 */

namespace {

constexpr size_t kMaxPassedFds = 4;

/** Send `len` bytes with `count` descriptors, at most kMaxPassedFds, in one SCM_RIGHTS.
 */
int send_with_fds(int fd, const void* data, size_t len, const int* fds, size_t count) {
  char control[CMSG_SPACE(sizeof(int) * kMaxPassedFds)] = {0};
  struct msghdr hdr = {};
  struct cmsghdr* cmsg;
  struct iovec iov;

  iov.iov_base = const_cast<void*>(data);
  iov.iov_len = len;
  hdr.msg_iov = &iov;
  hdr.msg_iovlen = 1;
  hdr.msg_control = control;
  hdr.msg_controllen = CMSG_SPACE(sizeof(int) * count);

  cmsg = CMSG_FIRSTHDR(&hdr);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int) * count);
  std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * count);

  return sendmsg(fd, &hdr, 0) == static_cast<ssize_t>(len) ? 0 : -errno;
}

/** Answer an IF_XSK_MAP_FD request with `count` descriptors. */
int send_map_fds(int fd, const int* fds, size_t count) {
  char data[1] = {' '};

  return send_with_fds(fd, data, sizeof(data), fds, count);
}

} /* namespace */

TEST_F(ClientWireTest, TheXskMapDescriptorArrivesAndCannotOutliveAnExec) {
  mtl_message_t sent;
  int result = 0;
  int passed = null_fd();
  wire_worker worker(conn_fd, [&]() { result = mtlm_xsk_map_fd(client, 3); });

  ASSERT_GE(passed, 0);
  ASSERT_EQ(wire_read(conn_fd, sent), 0);
  EXPECT_EQ(wire_type(sent), static_cast<uint32_t>(MTL_MSG_TYPE_IF_XSK_MAP_FD));
  EXPECT_EQ(ntohl(sent.body.if_msg.ifindex), 3u);
  ASSERT_EQ(send_map_fds(conn_fd, &passed, 1), 0);

  worker.join();
  close(passed);
  ASSERT_GE(result, 0) << std::strerror(-result);

  /* MTL runs a child process of its own, and the map of an AF_XDP port is not
   * for it. Without MSG_CMSG_CLOEXEC on the receive, every descriptor the
   * manager hands over stays open across each exec the caller makes. */
  int flags = fcntl(result, F_GETFD);
  EXPECT_GE(flags, 0);
  EXPECT_TRUE(flags & FD_CLOEXEC) << "the map descriptor survives an exec";
  close(result);
}

TEST_F(ClientWireTest, ARefusedAnswerLeavesNoDescriptorBehind) {
  int before = open_fd_count();
  mtl_message_t sent;
  int result = 0;
  int passed[2] = {null_fd(), null_fd()};
  wire_worker worker(conn_fd, [&]() { result = mtlm_xsk_map_fd(client, 3); });

  ASSERT_GT(before, 0);
  ASSERT_GE(passed[0], 0);
  ASSERT_GE(passed[1], 0);
  ASSERT_EQ(wire_read(conn_fd, sent), 0);

  /* Two descriptors in the control data. The client asked for one, so it must
   * refuse the answer, and it must close what the kernel already put in this
   * process. A manager of another version, or one with a fault, is enough to
   * make this happen, and an instance that asks in a loop then runs out of
   * descriptors. */
  ASSERT_EQ(send_map_fds(conn_fd, passed, 2), 0);

  worker.join();
  close(passed[0]);
  close(passed[1]);
  EXPECT_LT(result, 0) << "the client took an answer it could not read";
  if (result >= 0) close(result);

  EXPECT_EQ(open_fd_count(), before) << "a refused answer left a descriptor open";
}

/*
 * More descriptors than the control buffer holds.
 *
 * The kernel puts in this process as many as fit and sets MSG_CTRUNC. On a
 * 64-bit host two still fit, so the count check refuses this before the
 * MSG_CTRUNC check does. Every descriptor that did arrive must close.
 */
TEST_F(ClientWireTest, ATruncatedControlMessageLeavesNoDescriptorBehind) {
  int before = open_fd_count();
  int passed[kMaxPassedFds];
  mtl_message_t sent;
  int result = 0;

  ASSERT_GT(before, 0);
  for (int& fd : passed) {
    fd = null_fd();
    ASSERT_GE(fd, 0);
  }

  wire_worker worker(conn_fd, [&]() { result = mtlm_xsk_map_fd(client, 3); });
  ASSERT_EQ(wire_read(conn_fd, sent), 0);
  ASSERT_EQ(send_map_fds(conn_fd, passed, kMaxPassedFds), 0);
  worker.join();

  for (int fd : passed) close(fd);
  EXPECT_EQ(result, -ENOTSUP);
  if (result >= 0) close(result);
  EXPECT_EQ(open_fd_count(), before) << "a truncated answer left a descriptor open";
}

/*
 * Descriptors on the answer of a call that expects none.
 *
 * The kernel drops them today, because recv() gives no control room. The case
 * guards against a move of exchange() to recvmsg() that keeps them open.
 */
TEST_F(ClientWireTest, DescriptorsOnAnOrdinaryAnswerAreNotKept) {
  int before = open_fd_count();
  int passed[2] = {null_fd(), null_fd()};
  mtl_message_t sent;
  int result = -1;

  ASSERT_GT(before, 0);
  ASSERT_GE(passed[0], 0);
  ASSERT_GE(passed[1], 0);

  wire_worker worker(conn_fd, [&]() { result = mtlm_lcore_get(client, 1); });
  ASSERT_EQ(wire_read(conn_fd, sent), 0);
  mtl_message_t reply =
      wire_request(MTL_MSG_TYPE_RESPONSE, sizeof(mtl_response_message_t));
  ASSERT_EQ(send_with_fds(conn_fd, &reply, sizeof(reply), passed, 2), 0);
  worker.join();

  close(passed[0]);
  close(passed[1]);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(open_fd_count(), before) << "an ordinary answer left a descriptor open";
}

/*
 * Every refusal the client knows, one after the other on one connection.
 *
 * One leaked descriptor shows as a count off by one, and a leak checker sees
 * any heap block the refusals leave. Each round sends 0, 2 or 4 descriptors.
 */
TEST_F(ClientWireTest, HostileAnswersInALoopLeaveNothingBehind) {
  constexpr int kRounds = 3;
  int before = open_fd_count();

  ASSERT_GT(before, 0);

  for (int round = 0; round < kRounds; round++) {
    int passed[kMaxPassedFds];
    mtl_message_t sent;
    int result = 0;
    size_t count = static_cast<size_t>(round % 3) * 2; /* 0, 2 or 4 */

    for (size_t i = 0; i < count; i++) {
      passed[i] = null_fd();
      ASSERT_GE(passed[i], 0);
    }

    wire_worker worker(conn_fd, [&]() { result = mtlm_xsk_map_fd(client, 3); });
    ASSERT_EQ(wire_read(conn_fd, sent), 0) << "round " << round;
    if (count == 0) {
      char refusal = 'E';
      ASSERT_EQ(send(conn_fd, &refusal, 1, 0), 1) << "round " << round;
    } else {
      ASSERT_EQ(send_map_fds(conn_fd, passed, count), 0) << "round " << round;
    }
    worker.join();
    for (size_t i = 0; i < count; i++) close(passed[i]);
    EXPECT_EQ(result, -ENOTSUP) << "round " << round;

    /* A record with the wrong magic, which the client refuses. */
    result = 0;
    wire_worker other(conn_fd, [&]() { result = mtlm_lcore_get(client, 1); });
    ASSERT_EQ(wire_read(conn_fd, sent), 0) << "round " << round;
    mtl_message_t reply =
        wire_request(MTL_MSG_TYPE_RESPONSE, sizeof(mtl_response_message_t));
    reply.header.magic = htonl(0x0badf00d);
    ASSERT_EQ(send(conn_fd, &reply, sizeof(reply), 0),
              static_cast<ssize_t>(sizeof(reply)));
    other.join();
    EXPECT_EQ(result, -EBADMSG) << "round " << round;
  }

  EXPECT_EQ(open_fd_count(), before);
}

/*
 * A client that never connects.
 *
 * A socket file that nothing listens on is what a manager that crashed leaves,
 * and an instance that starts before the manager retries in a loop. Each failed
 * create must close the socket it opened.
 */
TEST_F(ClientWireTest, AFailedConnectLeavesNoDescriptorBehind) {
  std::string stale = dir + kStaleName;
  std::string missing = dir + "/missing.sock";
  std::string too_long = "/tmp/" + std::string(MTLM_SOCK_PATH_MAX, 'x');
  struct sockaddr_un addr = unix_addr(stale);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);

  ASSERT_GE(fd, 0);
  ASSERT_EQ(bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);
  close(fd); /* the file stays, and nothing answers on it */

  int before = open_fd_count();
  ASSERT_GT(before, 0);

  errno = 0;
  EXPECT_EQ(mtlm_client_create(stale.c_str()), nullptr);
  EXPECT_EQ(errno, ECONNREFUSED);
  EXPECT_EQ(mtlm_client_create(missing.c_str()), nullptr);
  EXPECT_EQ(errno, ENOENT);
  EXPECT_EQ(mtlm_client_create(too_long.c_str()), nullptr);
  EXPECT_EQ(errno, ENAMETOOLONG);
  EXPECT_FALSE(mtlm_manager_alive(stale.c_str()));

  EXPECT_EQ(open_fd_count(), before) << "a failed connect left a descriptor open";
}

/* A caller that passes on the NULL of a failed create gets an error, not a crash. */
TEST_F(ClientWireTest, EveryCallOnANullClientIsRefused) {
  struct mtlm_register_args args = {};
  struct mtlm_flow flow = {};
  uint32_t acked = 0;

  EXPECT_EQ(mtlm_register(nullptr, &args), -EINVAL);
  EXPECT_EQ(mtlm_heartbeat(nullptr, 1, &acked), -EINVAL);
  EXPECT_EQ(mtlm_lcore_get(nullptr, 1), -EINVAL);
  EXPECT_EQ(mtlm_lcore_put(nullptr, 1), -EINVAL);
  EXPECT_EQ(mtlm_queue_get(nullptr, 1), -EINVAL);
  EXPECT_EQ(mtlm_queue_put(nullptr, 1, 1), -EINVAL);
  EXPECT_EQ(mtlm_flow_add(nullptr, &flow), -EINVAL);
  EXPECT_EQ(mtlm_flow_del(nullptr, 1, 1), -EINVAL);
  EXPECT_EQ(mtlm_udp_dp_filter_add(nullptr, 1, 1), -EINVAL);
  EXPECT_EQ(mtlm_udp_dp_filter_del(nullptr, 1, 1), -EINVAL);
  EXPECT_EQ(mtlm_xsk_map_fd(nullptr, 1), -EINVAL);
  EXPECT_EQ(mtlm_client_fd(nullptr), -1);
  EXPECT_EQ(mtlm_client_sock_path(nullptr), nullptr);
  mtlm_client_destroy(nullptr);
}
