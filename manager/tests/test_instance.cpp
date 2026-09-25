/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * One connected instance: the record framing, the field validation, the
 * ownership checks and the cleanup on disconnect.
 *
 * Each case makes a socket pair. The instance gets one end, the test keeps the
 * other, so the test reads the very bytes a client would read.
 */

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "mtl_instance.hpp"
#include "mtlm_test_fakes.hpp"
#include "mtlm_test_wire.hpp"

namespace {

class InstanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    int fds[2];

    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    peer_fd = fds[0];
    wire_set_timeout(peer_fd, 5);

    netdev = std::make_shared<fake_netdev>();
    netdev->add_if(1, 4, 8);
    netdev->add_if(2, 4, 8);
    xdp = std::make_shared<fake_xdp_state>();
    registry.reset(new mtl_interface_registry(netdev, fake_xdp_factory(xdp)));
    instance.reset(new mtl_instance(fds[1], *registry, lcores));
  }

  void TearDown() override {
    instance.reset();
    registry.reset();
    if (peer_fd >= 0) close(peer_fd);
  }

  /** Hand one whole record to the instance. */
  int feed(const mtl_message_t& msg) {
    return instance->feed(reinterpret_cast<const char*>(&msg), MTL_MANAGER_MSG_SIZE);
  }

  /** Read the answer and return its response field. */
  int response(uint32_t expect_type = MTL_MSG_TYPE_RESPONSE) {
    mtl_message_t msg;

    int ret = wire_read(peer_fd, msg);
    if (ret < 0) return ret;
    if (wire_type(msg) != expect_type) return -EBADMSG;

    return wire_response(msg);
  }

  /** A register record for `count` interfaces starting at index 1. */
  mtl_message_t register_msg(uint16_t count) {
    mtl_message_t msg =
        wire_request(MTL_MSG_TYPE_REGISTER, sizeof(mtl_register_message_t));

    msg.body.register_msg.pid = static_cast<pid_t>(htonl(4321));
    msg.body.register_msg.uid = static_cast<uid_t>(htonl(1000));
    std::snprintf(msg.body.register_msg.hostname, sizeof(msg.body.register_msg.hostname),
                  "%s", "test-host");
    msg.body.register_msg.num_if = htons(count);
    for (uint16_t i = 0; i < count && i < MTL_MANAGER_MAX_IF; i++)
      msg.body.register_msg.ifindex[i] = htonl(i + 1u);

    return msg;
  }

  /** Register with one interface, which every other operation needs. */
  void do_register() {
    ASSERT_EQ(feed(register_msg(1)), 1);
    ASSERT_EQ(response(), 0);
    ASSERT_TRUE(instance->registered());
  }

  mtl_message_t if_msg(mtl_message_type_t type, unsigned int ifindex) {
    mtl_message_t msg = wire_request(type, sizeof(mtl_if_message_t));

    msg.body.if_msg.ifindex = htonl(ifindex);
    return msg;
  }

  mtl_message_t lcore_msg(mtl_message_type_t type, uint16_t lcore) {
    mtl_message_t msg = wire_request(type, sizeof(mtl_lcore_message_t));

    msg.body.lcore_msg.lcore = htons(lcore);
    return msg;
  }

  int peer_fd = -1;
  mtl_lcore lcores;
  std::shared_ptr<fake_netdev> netdev;
  std::shared_ptr<fake_xdp_state> xdp;
  std::unique_ptr<mtl_interface_registry> registry;
  std::unique_ptr<mtl_instance> instance;
};

} /* namespace */

TEST_F(InstanceTest, RegisterReportsTheClientFields) {
  do_register();

  EXPECT_EQ(instance->get_pid(), 4321);
  EXPECT_EQ(instance->get_uid(), 1000);
  /* The field is 64 padded bytes, so the name must stop at the first zero. */
  EXPECT_EQ(instance->get_hostname(), "test-host");
}

TEST_F(InstanceTest, RegisterWithNoHostnameStillNamesTheClient) {
  mtl_message_t msg = register_msg(1);

  std::memset(msg.body.register_msg.hostname, 0, sizeof(msg.body.register_msg.hostname));
  ASSERT_EQ(feed(msg), 1);
  EXPECT_EQ(response(), 0);
  EXPECT_EQ(instance->get_hostname(), "unknown");
}

TEST_F(InstanceTest, RegisterRefusesTooManyInterfaces) {
  mtl_message_t msg = register_msg(1);

  /* The record holds MTL_MANAGER_MAX_IF indexes. A larger count used to read
   * past the end of the record. */
  msg.body.register_msg.num_if = htons(MTL_MANAGER_MAX_IF + 1);
  ASSERT_EQ(feed(msg), 1);
  EXPECT_EQ(response(), -EINVAL);
  EXPECT_FALSE(instance->registered());
}

TEST_F(InstanceTest, RegisterRefusesAnUnknownInterface) {
  mtl_message_t msg = register_msg(1);

  msg.body.register_msg.ifindex[0] = htonl(99);
  ASSERT_EQ(feed(msg), 1);
  EXPECT_EQ(response(), -ENODEV);
  EXPECT_FALSE(instance->registered());
}

TEST_F(InstanceTest, RegisterNeedsAnXdpProgram) {
  xdp->attach_ret = -ENOTSUP;

  /* The interfaces of a register message are the AF_XDP ports of the instance,
   * so one that cannot take the program is no use to it. */
  ASSERT_EQ(feed(register_msg(1)), 1);
  EXPECT_EQ(response(), -ENODEV);
  EXPECT_FALSE(instance->registered());
}

TEST_F(InstanceTest, EveryOperationNeedsARegisteredInstance) {
  ASSERT_EQ(feed(lcore_msg(MTL_MSG_TYPE_GET_LCORE, 3)), 1);
  EXPECT_EQ(response(), -EPERM);

  ASSERT_EQ(feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, 1)), 1);
  EXPECT_EQ(response(MTL_MSG_TYPE_IF_QUEUE_ID), -EPERM);

  ASSERT_EQ(feed(if_msg(MTL_MSG_TYPE_IF_ADD_FLOW, 1)), 1);
  EXPECT_EQ(response(MTL_MSG_TYPE_IF_FLOW_ID), -EPERM);

  EXPECT_EQ(lcores.used_count(), 0u);
}

TEST_F(InstanceTest, HeartbeatNeedsNoRegistration) {
  mtl_message_t msg =
      wire_request(MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));

  msg.body.heartbeat_msg.seq = htonl(0xdeadbeef);
  ASSERT_EQ(feed(msg), 1);

  mtl_message_t reply;
  ASSERT_EQ(wire_read(peer_fd, reply), 0);
  EXPECT_EQ(wire_type(reply), static_cast<uint32_t>(MTL_MSG_TYPE_HEARTBEAT_ACK));
  /* A client uses the number to match the answer to the question. */
  EXPECT_EQ(ntohl(reply.body.heartbeat_msg.seq), 0xdeadbeefu);
}

TEST_F(InstanceTest, AnUnknownTypeGetsAnAnswer) {
  mtl_message_t msg = wire_request(static_cast<mtl_message_type_t>(4242), 0);

  /* Silence used to leave a client waiting for its own timeout. */
  ASSERT_EQ(feed(msg), 1);
  EXPECT_EQ(response(), -ENOTSUP);
}

TEST_F(InstanceTest, BadMagicDropsTheConnection) {
  mtl_message_t msg = wire_request(MTL_MSG_TYPE_HEARTBEAT, 0);

  msg.header.magic = htonl(0x12345678);
  EXPECT_EQ(feed(msg), -EBADMSG);
}

TEST_F(InstanceTest, ASplitRecordWaitsForTheRest) {
  mtl_message_t msg = register_msg(1);
  const char* bytes = reinterpret_cast<const char*>(&msg);
  size_t split = MTL_MANAGER_MSG_SIZE / 2;

  /* A stream socket may deliver half a record. The old code read 256 bytes and
   * handled whatever arrived, so a split record was a lost request. */
  EXPECT_EQ(instance->feed(bytes, split), 0);
  EXPECT_FALSE(instance->registered());

  EXPECT_EQ(instance->feed(bytes + split, MTL_MANAGER_MSG_SIZE - split), 1);
  EXPECT_EQ(response(), 0);
  EXPECT_TRUE(instance->registered());
}

TEST_F(InstanceTest, CoalescedRecordsAreAllHandled) {
  std::vector<char> stream;
  mtl_message_t first = register_msg(1);
  mtl_message_t second = lcore_msg(MTL_MSG_TYPE_GET_LCORE, 5);
  mtl_message_t third = lcore_msg(MTL_MSG_TYPE_GET_LCORE, 6);

  const char* p = reinterpret_cast<const char*>(&first);
  stream.insert(stream.end(), p, p + MTL_MANAGER_MSG_SIZE);
  p = reinterpret_cast<const char*>(&second);
  stream.insert(stream.end(), p, p + MTL_MANAGER_MSG_SIZE);
  p = reinterpret_cast<const char*>(&third);
  stream.insert(stream.end(), p, p + MTL_MANAGER_MSG_SIZE);

  EXPECT_EQ(instance->feed(stream.data(), stream.size()), 3);
  EXPECT_EQ(response(), 0);
  EXPECT_EQ(response(), 0);
  EXPECT_EQ(response(), 0);
  EXPECT_EQ(lcores.used_count(), 2u);
}

TEST_F(InstanceTest, LcoreGetAndPut) {
  do_register();

  ASSERT_EQ(feed(lcore_msg(MTL_MSG_TYPE_GET_LCORE, 3)), 1);
  EXPECT_EQ(response(), 0);
  EXPECT_TRUE(lcores.is_used(3));
  EXPECT_EQ(instance->lcore_count(), 1u);

  ASSERT_EQ(feed(lcore_msg(MTL_MSG_TYPE_PUT_LCORE, 3)), 1);
  EXPECT_EQ(response(), 0);
  EXPECT_FALSE(lcores.is_used(3));
  EXPECT_EQ(instance->lcore_count(), 0u);
}

TEST_F(InstanceTest, AnInstanceCannotPutAnLcoreItDoesNotHold) {
  do_register();

  /* Another instance holds it. */
  ASSERT_EQ(lcores.get_lcore(9), 0);

  ASSERT_EQ(feed(lcore_msg(MTL_MSG_TYPE_PUT_LCORE, 9)), 1);
  EXPECT_EQ(response(), -EINVAL);
  /* The other instance must still hold it. */
  EXPECT_TRUE(lcores.is_used(9));
}

TEST_F(InstanceTest, ABusyLcoreIsReported) {
  do_register();
  ASSERT_EQ(lcores.get_lcore(4), 0);

  ASSERT_EQ(feed(lcore_msg(MTL_MSG_TYPE_GET_LCORE, 4)), 1);
  EXPECT_EQ(response(), -EBUSY);
  EXPECT_EQ(instance->lcore_count(), 0u);
}

TEST_F(InstanceTest, QueueGetAndPut) {
  do_register();

  ASSERT_EQ(feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, 1)), 1);
  EXPECT_EQ(response(MTL_MSG_TYPE_IF_QUEUE_ID), 1);
  EXPECT_EQ(instance->queue_count(1), 1u);

  mtl_message_t put = if_msg(MTL_MSG_TYPE_IF_PUT_QUEUE, 1);
  put.body.if_msg.queue_id = htons(1);
  ASSERT_EQ(feed(put), 1);
  EXPECT_EQ(response(), 0);
  EXPECT_EQ(instance->queue_count(1), 0u);
}

TEST_F(InstanceTest, AnInstanceCannotPutAQueueItDoesNotHold) {
  do_register();

  mtl_message_t put = if_msg(MTL_MSG_TYPE_IF_PUT_QUEUE, 1);
  put.body.if_msg.queue_id = htons(2);
  ASSERT_EQ(feed(put), 1);
  EXPECT_EQ(response(), -EINVAL);
}

TEST_F(InstanceTest, QueueGetOnAnUnknownInterface) {
  do_register();

  ASSERT_EQ(feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, 99)), 1);
  EXPECT_EQ(response(MTL_MSG_TYPE_IF_QUEUE_ID), -ENODEV);
}

TEST_F(InstanceTest, FlowAddPassesTheAddressesUnchanged) {
  do_register();

  mtl_message_t msg = if_msg(MTL_MSG_TYPE_IF_ADD_FLOW, 1);
  msg.body.if_msg.queue_id = htons(2);
  msg.body.if_msg.flow_type = htonl(0x02);
  /* An address is already network byte order on the wire, and ethtool wants
   * that order too, so the manager must not swap it. */
  msg.body.if_msg.src_ip = 0x0100000a;
  msg.body.if_msg.dst_ip = 0x0200000a;
  msg.body.if_msg.src_port = htons(1234);
  msg.body.if_msg.dst_port = htons(5678);

  ASSERT_EQ(feed(msg), 1);
  int flow_id = response(MTL_MSG_TYPE_IF_FLOW_ID);
  EXPECT_EQ(flow_id, 7);
  EXPECT_EQ(instance->flow_count(1), 1u);

  ASSERT_EQ(netdev->inserted.size(), 1u);
  EXPECT_EQ(netdev->inserted[0].src_ip, 0x0100000au);
  EXPECT_EQ(netdev->inserted[0].dst_ip, 0x0200000au);
  /* A port is host byte order by the time it reaches the device. */
  EXPECT_EQ(netdev->inserted[0].src_port, 1234);
  EXPECT_EQ(netdev->inserted[0].dst_port, 5678);
  EXPECT_EQ(netdev->inserted[0].queue_id, 2);

  mtl_message_t del = if_msg(MTL_MSG_TYPE_IF_DEL_FLOW, 1);
  del.body.if_msg.flow_id = htonl(static_cast<uint32_t>(flow_id));
  ASSERT_EQ(feed(del), 1);
  EXPECT_EQ(response(), 0);
  EXPECT_EQ(instance->flow_count(1), 0u);
}

TEST_F(InstanceTest, AnInstanceCannotDeleteAFlowItDoesNotHold) {
  do_register();

  mtl_message_t del = if_msg(MTL_MSG_TYPE_IF_DEL_FLOW, 1);
  del.body.if_msg.flow_id = htonl(5);
  ASSERT_EQ(feed(del), 1);
  EXPECT_EQ(response(), -EINVAL);
  /* The refusal must not reach the device. */
  EXPECT_EQ(netdev->delete_calls, 0);
}

TEST_F(InstanceTest, UdpFilterGoesToTheXdpMap) {
  do_register();

  mtl_message_t msg =
      wire_request(MTL_MSG_TYPE_ADD_UDP_DP_FILTER, sizeof(mtl_udp_dp_filter_message_t));
  msg.body.udp_dp_filter_msg.ifindex = htonl(1);
  msg.body.udp_dp_filter_msg.port = htons(20000);

  ASSERT_EQ(feed(msg), 1);
  EXPECT_EQ(response(), 0);
  ASSERT_EQ(xdp->filter_calls.size(), 1u);
  EXPECT_EQ(xdp->filter_calls[0].first, 20000);
  EXPECT_TRUE(xdp->filter_calls[0].second);

  mtl_message_t del =
      wire_request(MTL_MSG_TYPE_DEL_UDP_DP_FILTER, sizeof(mtl_udp_dp_filter_message_t));
  del.body.udp_dp_filter_msg.ifindex = htonl(1);
  del.body.udp_dp_filter_msg.port = htons(20000);
  ASSERT_EQ(feed(del), 1);
  EXPECT_EQ(response(), 0);
  ASSERT_EQ(xdp->filter_calls.size(), 2u);
  EXPECT_FALSE(xdp->filter_calls[1].second);
}

TEST_F(InstanceTest, XskMapFdSendsTheDescriptor) {
  int dup_fd = dup(peer_fd);

  ASSERT_GE(dup_fd, 0);
  xdp->map_fd = dup_fd;
  do_register();

  ASSERT_EQ(feed(if_msg(MTL_MSG_TYPE_IF_XSK_MAP_FD, 1)), 1);

  char data[1] = {0};
  char control[CMSG_SPACE(sizeof(int))];
  struct iovec iov = {};
  struct msghdr hdr = {};

  std::memset(control, 0, sizeof(control));
  iov.iov_base = data;
  iov.iov_len = sizeof(data);
  hdr.msg_iov = &iov;
  hdr.msg_iovlen = 1;
  hdr.msg_control = control;
  hdr.msg_controllen = sizeof(control);

  ASSERT_GT(recvmsg(peer_fd, &hdr, 0), 0);
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr);
  ASSERT_NE(cmsg, nullptr);
  EXPECT_EQ(cmsg->cmsg_type, SCM_RIGHTS);

  int got = -1;
  std::memcpy(&got, CMSG_DATA(cmsg), sizeof(got));
  EXPECT_GE(got, 0);
  close(got);
  close(dup_fd);
}

TEST_F(InstanceTest, XskMapFdWithNoProgramStillAnswers) {
  xdp->attach_ret = -ENOTSUP;

  /* SCM_RIGHTS cannot carry -1, so the old code failed the sendmsg with EBADF
   * and the client waited for a message that never came. */
  ASSERT_EQ(feed(if_msg(MTL_MSG_TYPE_IF_XSK_MAP_FD, 1)), 1);

  char data[1] = {0};
  char control[CMSG_SPACE(sizeof(int))];
  struct iovec iov = {};
  struct msghdr hdr = {};

  std::memset(control, 0, sizeof(control));
  iov.iov_base = data;
  iov.iov_len = sizeof(data);
  hdr.msg_iov = &iov;
  hdr.msg_iovlen = 1;
  hdr.msg_control = control;
  hdr.msg_controllen = sizeof(control);

  ASSERT_GT(recvmsg(peer_fd, &hdr, 0), 0);
  /* No control data is how the client learns there is no descriptor. */
  EXPECT_EQ(CMSG_FIRSTHDR(&hdr), nullptr);
}

TEST_F(InstanceTest, DisconnectGivesEveryResourceBack) {
  do_register();

  ASSERT_EQ(feed(lcore_msg(MTL_MSG_TYPE_GET_LCORE, 3)), 1);
  ASSERT_EQ(response(), 0);
  ASSERT_EQ(feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, 1)), 1);
  ASSERT_EQ(response(MTL_MSG_TYPE_IF_QUEUE_ID), 1);

  mtl_message_t flow = if_msg(MTL_MSG_TYPE_IF_ADD_FLOW, 1);
  flow.body.if_msg.queue_id = htons(1);
  ASSERT_EQ(feed(flow), 1);
  ASSERT_GT(response(MTL_MSG_TYPE_IF_FLOW_ID), 0);

  auto interface = registry->get(1, false);
  ASSERT_NE(interface, nullptr);

  instance.reset();

  EXPECT_FALSE(lcores.is_used(3));
  EXPECT_EQ(lcores.used_count(), 0u);
  /* The queue and the rule come back, so a client that dies leaves nothing. */
  EXPECT_EQ(interface->get_queue(), 1);
  EXPECT_TRUE(netdev->ifaces[1].rules.empty());
}

TEST_F(InstanceTest, TheResponseRecordCarriesNothingElse) {
  do_register();

  mtl_message_t reply;
  ASSERT_EQ(feed(lcore_msg(MTL_MSG_TYPE_GET_LCORE, 3)), 1);
  ASSERT_EQ(wire_read(peer_fd, reply), 0);

  EXPECT_EQ(ntohl(reply.header.magic), static_cast<uint32_t>(MTL_MANAGER_MAGIC));
  EXPECT_EQ(ntohl(reply.header.body_len), sizeof(mtl_response_message_t));

  /* Every byte after the response field must be zero. Sending the stack frame
   * of the manager to a client is a leak. */
  const char* body = reinterpret_cast<const char*>(&reply.body);
  for (size_t i = sizeof(mtl_response_message_t); i < sizeof(reply.body); i++)
    EXPECT_EQ(body[i], 0) << "byte " << i << " of the body is not zero";
}
