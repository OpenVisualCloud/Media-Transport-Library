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

class ClientWireTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mtlm-client-wire-XXXXXX";
    struct sockaddr_un addr = {};

    ASSERT_NE(mkdtemp(pattern), nullptr);
    dir = pattern;
    path = dir + "/mtl_manager.sock";

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
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
   * is the order ethtool wants, so the client must pass it through untouched. */
  EXPECT_EQ(sent.body.if_msg.src_ip, 0x0100000au);
  EXPECT_EQ(sent.body.if_msg.dst_ip, 0x0200000au);
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
  std::thread worker(
      [&]() { EXPECT_EQ(mtlm_heartbeat(client, 0xdeadbeefu, &acked), 0); });

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
  std::thread worker([&]() { result = mtlm_lcore_get(client, 1); });

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
  std::thread worker([&]() { result = mtlm_lcore_get(client, 1); });

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
  std::thread worker([&]() { result = mtlm_lcore_get(client, 1); });

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
