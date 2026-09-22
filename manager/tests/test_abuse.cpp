/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * Cases written to break the manager, not to show that it works.
 *
 * The other test files drive one well behaved client. These drive a hostile
 * one: a client that never registers, a client that gives back a resource it
 * never took, a client that dies while it holds something, two clients that
 * fight over the same interface, and a stream of records with random bytes in
 * every field.
 *
 * Build the manager with a sanitizer and run this file, because some of the
 * faults these cases reach are a leak or an invalid read that no return value
 * reports:
 *
 *   CXXFLAGS=-fsanitize=undefined LDFLAGS=-fsanitize=undefined \
 *     meson setup build_san -Dbuildtype=debug -Denable_asan=true
 *   ninja -C build_san
 *   LD_PRELOAD=$(gcc -print-file-name=libasan.so) \
 *     UBSAN_OPTIONS=halt_on_error=1 ./build_san/tests/MtlManagerTest \
 *     --gtest_filter='Abuse*'
 *
 * Every device call goes through a fake, so the cases need no NIC, no root and
 * no libxdp.
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
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "mtl_instance.hpp"
#include "mtlm_server.hpp"
#include "mtlm_test_fakes.hpp"
#include "mtlm_test_wire.hpp"

namespace {

/* Interfaces the fake netdev holds. Interface 1 also starts with a rule in it,
 * so a case can prove that the manager did not clear the rule table. */
constexpr unsigned int kIf = 1;
constexpr unsigned int kOtherIf = 2;
constexpr uint32_t kForeignRule = 3;
constexpr uint16_t kFilterPort = 5000;

/**
 * One connected client of a shared registry.
 *
 * Several of these live at once, because a fault that only shows with two
 * clients, such as one client releasing the resource of another, cannot be
 * reached with one.
 */
class abuse_client {
 public:
  abuse_client(mtl_interface_registry& registry, mtl_lcore& lcores) {
    int fds[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return;
    peer_fd = fds[0];
    wire_set_timeout(peer_fd, 5);
    instance.reset(new mtl_instance(fds[1], registry, lcores));
  }

  ~abuse_client() {
    close_instance();
    if (peer_fd >= 0) close(peer_fd);
  }

  abuse_client(const abuse_client&) = delete;
  abuse_client& operator=(const abuse_client&) = delete;

  /** Drop the instance, which is what the server does when a client dies. */
  void close_instance() {
    instance.reset();
  }

  bool ok() const {
    return instance != nullptr;
  }

  int feed(const mtl_message_t& msg) {
    return instance->feed(reinterpret_cast<const char*>(&msg), MTL_MANAGER_MSG_SIZE);
  }

  int feed(const void* bytes, size_t len) {
    return instance->feed(static_cast<const char*>(bytes), len);
  }

  /** Response field of the next record, or a negative errno. */
  int response(uint32_t expect_type = MTL_MSG_TYPE_RESPONSE) {
    mtl_message_t msg;

    int ret = wire_read(peer_fd, msg);
    if (ret < 0) return ret;
    if (wire_type(msg) != expect_type) return -EBADMSG;

    return wire_response(msg);
  }

  /** Whether any byte is waiting. A refusal must still answer something. */
  bool has_answer() {
    char byte = 0;
    ssize_t got = recv(peer_fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);

    return got == 1;
  }

  /**
   * Read the answer to an IF_XSK_MAP_FD request the way the client library
   * does: one data byte, and the descriptor in the control data.
   *
   * @return The descriptor, or -ENOTSUP when the manager passed none.
   */
  int xsk_map_fd() {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    char data[1] = {0};
    struct msghdr hdr = {};
    struct iovec iov;
    int fd = -1;

    iov.iov_base = data;
    iov.iov_len = sizeof(data);
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = control;
    hdr.msg_controllen = sizeof(control);

    ssize_t got = recvmsg(peer_fd, &hdr, 0);
    if (got < 0) return -errno;
    if (got == 0) return -ECONNRESET;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr);
    if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
      return -ENOTSUP;

    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
  }

  int peer_fd = -1;
  std::unique_ptr<mtl_instance> instance;
};

class AbuseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    netdev = std::make_shared<fake_netdev>();
    netdev->add_if(kIf, 4, 8);
    netdev->add_if(kOtherIf, 4, 8);
    /* A rule someone else installed. The manager clears the whole table when it
     * takes an interface, so this is how a case sees that it did. */
    netdev->ifaces[kIf].rules.push_back(kForeignRule);
    xdp = std::make_shared<fake_xdp_state>();
    /* The instance passes this descriptor over SCM_RIGHTS, so it must be one
     * this process owns. */
    ASSERT_GE(xdp->open_real_map_fd(), 0);
    registry.reset(new mtl_interface_registry(netdev, fake_xdp_factory(xdp)));
  }

  void TearDown() override {
    clients.clear();
    registry.reset();
  }

  abuse_client& new_client() {
    clients.push_back(std::unique_ptr<abuse_client>(new abuse_client(*registry, lcores)));
    return *clients.back();
  }

  static mtl_message_t register_msg(unsigned int ifindex) {
    mtl_message_t msg =
        wire_request(MTL_MSG_TYPE_REGISTER, sizeof(mtl_register_message_t));

    msg.body.register_msg.pid = static_cast<pid_t>(htonl(4321));
    msg.body.register_msg.uid = static_cast<uid_t>(htonl(1000));
    std::snprintf(msg.body.register_msg.hostname, sizeof(msg.body.register_msg.hostname),
                  "%s", "abuse-host");
    msg.body.register_msg.num_if = htons(1);
    msg.body.register_msg.ifindex[0] = htonl(ifindex);
    return msg;
  }

  static mtl_message_t if_msg(mtl_message_type_t type, unsigned int ifindex) {
    mtl_message_t msg = wire_request(type, sizeof(mtl_if_message_t));

    msg.body.if_msg.ifindex = htonl(ifindex);
    return msg;
  }

  static mtl_message_t filter_msg(mtl_message_type_t type, unsigned int ifindex,
                                  uint16_t port) {
    mtl_message_t msg = wire_request(type, sizeof(mtl_udp_dp_filter_message_t));

    msg.body.udp_dp_filter_msg.ifindex = htonl(ifindex);
    msg.body.udp_dp_filter_msg.port = htons(port);
    return msg;
  }

  /** Register `client` on `ifindex` and check that it took. */
  void do_register(abuse_client& client, unsigned int ifindex = kIf) {
    ASSERT_TRUE(client.ok());
    ASSERT_EQ(client.feed(register_msg(ifindex)), 1);
    ASSERT_EQ(client.response(), 0);
  }

  /** Number of (port, add) calls the fake XDP program saw for `port`. */
  size_t filter_calls(uint16_t port, bool add) const {
    size_t count = 0;

    for (const auto& call : xdp->filter_calls)
      if (call.first == port && call.second == add) count++;

    return count;
  }

  mtl_lcore lcores;
  std::shared_ptr<fake_netdev> netdev;
  std::shared_ptr<fake_xdp_state> xdp;
  std::unique_ptr<mtl_interface_registry> registry;
  std::vector<std::unique_ptr<abuse_client>> clients;
};

/**
 * A real server on a path below /tmp, with its loop in a thread.
 *
 * A case that needs the loop itself, and not one instance, uses this. The
 * messages below name no interface, so the case still needs no NIC and no root.
 */
class AbuseServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mtlm-abuse-XXXXXX";

    ASSERT_NE(mkdtemp(pattern), nullptr);
    dir = pattern;
    path = dir + "/mtl_manager.sock";

    mtlm_server_config cfg;
    cfg.sock_path = path;
    server.reset(new mtlm_server(cfg));
    ASSERT_EQ(server->setup(), 0);
    worker = std::thread([this]() { this->server->run(); });
  }

  void TearDown() override {
    /* Close every client first. A server that waits in send() to one of them
     * only comes back when that socket goes away, and the join below would
     * otherwise never return. */
    for (int fd : conns)
      if (fd >= 0) close(fd);
    server->stop();
    if (worker.joinable()) worker.join();
    server.reset();
    unlink(path.c_str());
    rmdir(dir.c_str());
  }

  /** Connect one client. The fixture closes it. */
  int connect_client() {
    struct sockaddr_un addr = {};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) return -errno;
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      int ret = -errno;
      close(fd);
      return ret;
    }
    conns.push_back(fd);
    return fd;
  }

  static mtl_message_t heartbeat(uint32_t seq) {
    mtl_message_t msg =
        wire_request(MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));

    msg.body.heartbeat_msg.seq = htonl(seq);
    return msg;
  }

  std::string dir;
  std::string path;
  std::unique_ptr<mtlm_server> server;
  std::thread worker;
  std::vector<int> conns;
};

} /* namespace */

/*
 * A UDP filter port is a resource of the client, the same as an lcore, a queue
 * and a flow rule. A client that dies while it holds one must give it back.
 *
 * The second client keeps the interface alive on purpose. Without it the
 * interface goes away with the first client and the whole XDP program with it,
 * which hides the fault: the port stays in the udp4_dp_filter map, and the
 * reference count of the port stays raised, for as long as MTL keeps using that
 * interface.
 */
TEST_F(AbuseTest, AFilterPortGoesBackWhenTheClientDies) {
  abuse_client& owner = new_client();
  abuse_client& other = new_client();

  do_register(owner);
  do_register(other);

  ASSERT_EQ(owner.feed(filter_msg(MTL_MSG_TYPE_ADD_UDP_DP_FILTER, kIf, kFilterPort)), 1);
  ASSERT_EQ(owner.response(), 0);
  ASSERT_EQ(filter_calls(kFilterPort, true), 1u);

  owner.close_instance();

  EXPECT_TRUE(xdp->attached) << "the second client must keep the interface alive";
  EXPECT_EQ(filter_calls(kFilterPort, false), 1u)
      << "the port stayed in udp4_dp_filter after the client that added it died";
}

/*
 * Every other release path refuses a resource the client does not hold:
 * put_lcore, if_put_queue and if_del_flow all check first. The filter port must
 * do the same, because the manager counts the references to a port. One delete
 * too many takes the port out of the map, and the client that really uses it
 * stops receiving.
 */
TEST_F(AbuseTest, OneClientCannotDeleteTheFilterPortOfAnother) {
  abuse_client& owner = new_client();
  abuse_client& thief = new_client();

  do_register(owner);
  do_register(thief);

  ASSERT_EQ(owner.feed(filter_msg(MTL_MSG_TYPE_ADD_UDP_DP_FILTER, kIf, kFilterPort)), 1);
  ASSERT_EQ(owner.response(), 0);

  ASSERT_EQ(thief.feed(filter_msg(MTL_MSG_TYPE_DEL_UDP_DP_FILTER, kIf, kFilterPort)), 1);
  EXPECT_EQ(thief.response(), -EINVAL);
  EXPECT_EQ(filter_calls(kFilterPort, false), 0u)
      << "a client deleted a filter port it never added";
}

/* A client that adds a port twice holds it twice, and one delete leaves it. */
TEST_F(AbuseTest, ADoubleDeleteOfItsOwnFilterPortIsRefused) {
  abuse_client& owner = new_client();

  do_register(owner);

  ASSERT_EQ(owner.feed(filter_msg(MTL_MSG_TYPE_ADD_UDP_DP_FILTER, kIf, kFilterPort)), 1);
  ASSERT_EQ(owner.response(), 0);
  ASSERT_EQ(owner.feed(filter_msg(MTL_MSG_TYPE_DEL_UDP_DP_FILTER, kIf, kFilterPort)), 1);
  ASSERT_EQ(owner.response(), 0);

  ASSERT_EQ(owner.feed(filter_msg(MTL_MSG_TYPE_DEL_UDP_DP_FILTER, kIf, kFilterPort)), 1);
  EXPECT_EQ(owner.response(), -EINVAL);
  EXPECT_EQ(filter_calls(kFilterPort, false), 1u);
}

/*
 * One port added twice needs two deletes, and the accounting must not grow with
 * the number of adds.
 *
 * The interface counts the references to a port, so the instance has to count
 * them the same way. It keeps a count per port and not a copy per add, because a
 * client can send this message as often as it likes and the manager must not
 * allocate for each one.
 */
TEST_F(AbuseTest, AFilterPortAddedTwiceNeedsTwoDeletes) {
  constexpr int kAdds = 1000;
  abuse_client& owner = new_client();

  do_register(owner);

  for (int i = 0; i < kAdds; i++) {
    ASSERT_EQ(owner.feed(filter_msg(MTL_MSG_TYPE_ADD_UDP_DP_FILTER, kIf, kFilterPort)), 1)
        << "add " << i;
    ASSERT_EQ(owner.response(), 0) << "add " << i;
  }
  EXPECT_EQ(owner.instance->filter_count(kIf), static_cast<size_t>(kAdds));

  for (int i = 0; i < kAdds; i++) {
    ASSERT_EQ(owner.feed(filter_msg(MTL_MSG_TYPE_DEL_UDP_DP_FILTER, kIf, kFilterPort)), 1)
        << "delete " << i;
    ASSERT_EQ(owner.response(), 0) << "delete " << i;
  }
  EXPECT_EQ(owner.instance->filter_count(kIf), 0u);

  /* One delete more than the adds. */
  ASSERT_EQ(owner.feed(filter_msg(MTL_MSG_TYPE_DEL_UDP_DP_FILTER, kIf, kFilterPort)), 1);
  EXPECT_EQ(owner.response(), -EINVAL);
  EXPECT_EQ(filter_calls(kFilterPort, false), static_cast<size_t>(kAdds));
}

/*
 * Taking over an interface is not a read: the manager attaches an XDP program
 * to it and deletes every receive flow rule the interface holds, the rules of
 * other programs included. A client that never registered must not be able to
 * ask for that, and IF_XSK_MAP_FD was the one message type that let it.
 */
TEST_F(AbuseTest, XskMapFdFromAnUnregisteredClientTakesNoInterface) {
  abuse_client& stranger = new_client();

  ASSERT_TRUE(stranger.ok());
  ASSERT_EQ(stranger.feed(if_msg(MTL_MSG_TYPE_IF_XSK_MAP_FD, kIf)), 1);

  EXPECT_EQ(stranger.xsk_map_fd(), -ENOTSUP);
  EXPECT_EQ(xdp->attach_calls, 0) << "an unregistered client loaded an XDP program";
  EXPECT_EQ(netdev->ifaces[kIf].rules, std::vector<uint32_t>{kForeignRule})
      << "an unregistered client cleared the flow rules of an interface";
  EXPECT_EQ(registry->live_count(), 0u);
}

/* The same check on a client that did register, so the refusal is about the
 * register and not about the interface. */
TEST_F(AbuseTest, XskMapFdFromARegisteredClientStillWorks) {
  abuse_client& client = new_client();

  do_register(client);
  ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_XSK_MAP_FD, kIf)), 1);

  int fd = client.xsk_map_fd();
  EXPECT_GE(fd, 0);
  if (fd >= 0) close(fd);
}

/*
 * A record with a type no version of the protocol ever had.
 *
 * The answer must be -ENOTSUP and the connection must live, which the value
 * below reports. The other half of this case is the sanitizer: the type field
 * carries a byte-swapped number, so a build that reads it as the enum it was
 * declared to be makes an invalid enum load of it, and UBSan stops the run.
 */
TEST_F(AbuseTest, AnUnknownMessageTypeIsAnswered) {
  abuse_client& client = new_client();
  mtl_message_t msg = wire_request(MTL_MSG_TYPE_REGISTER, sizeof(mtl_if_message_t));
  uint32_t unknown = htonl(0xFFFFFFFFu);

  do_register(client);

  std::memcpy(&msg.header.type, &unknown, sizeof(unknown));
  ASSERT_EQ(client.feed(msg), 1);
  EXPECT_EQ(client.response(), -ENOTSUP);

  /* Still serving. */
  ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, kIf)), 1);
  EXPECT_GT(client.response(MTL_MSG_TYPE_IF_QUEUE_ID), 0);
}

/*
 * Random bytes in every field of a well framed record.
 *
 * The magic and the length stay right, so each record reaches dispatch() and
 * every handler runs on values no client library would build: an ifindex that
 * names nothing, a queue id larger than the table, a port of zero, a num_if
 * past the end of the array. The case asserts only that the instance keeps
 * framing; the sanitizer is what reads the rest.
 */
TEST_F(AbuseTest, RandomFieldsInWellFramedRecords) {
  abuse_client& client = new_client();
  std::mt19937 rng(20260922); /* fixed, so a failure repeats */
  const mtl_message_type_t types[] = {
      MTL_MSG_TYPE_REGISTER,          MTL_MSG_TYPE_HEARTBEAT,
      MTL_MSG_TYPE_GET_LCORE,         MTL_MSG_TYPE_PUT_LCORE,
      MTL_MSG_TYPE_ADD_UDP_DP_FILTER, MTL_MSG_TYPE_DEL_UDP_DP_FILTER,
      MTL_MSG_TYPE_IF_GET_QUEUE,      MTL_MSG_TYPE_IF_PUT_QUEUE,
      MTL_MSG_TYPE_IF_ADD_FLOW,       MTL_MSG_TYPE_IF_DEL_FLOW,
  };

  do_register(client);
  /* Drain the answer of every record, or the socket buffer fills and the
   * instance blocks in send(). */
  wire_set_timeout(client.peer_fd, 1);

  for (int round = 0; round < 2000; round++) {
    mtl_message_t msg = wire_request(types[rng() % (sizeof(types) / sizeof(types[0]))],
                                     rng() % 0xFFFFFFFFu);
    auto* body = reinterpret_cast<uint8_t*>(&msg.body);

    for (size_t i = 0; i < sizeof(msg.body); i++)
      body[i] = static_cast<uint8_t>(rng() & 0xFF);

    ASSERT_EQ(client.feed(msg), 1) << "round " << round;

    /* Read whatever came back. A handler that refuses still answers, and
     * IF_XSK_MAP_FD is not in the list, so every answer is one record. */
    mtl_message_t reply;
    ASSERT_EQ(wire_read(client.peer_fd, reply), 0) << "round " << round;
    ASSERT_EQ(ntohl(reply.header.magic), static_cast<uint32_t>(MTL_MANAGER_MAGIC))
        << "round " << round;
  }
}

/*
 * The stream split at every offset, with the records still whole.
 *
 * mtl_instance keeps the tail of a partial record until the rest arrives. A
 * fault here reads the buffer past what it holds, which is why the case walks
 * every split point rather than one.
 */
TEST_F(AbuseTest, EverySplitPointOfATwoRecordStream) {
  for (size_t split = 1; split < 2 * MTL_MANAGER_MSG_SIZE; split++) {
    abuse_client& client = new_client();
    mtl_message_t pair[2] = {register_msg(kIf), if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, kIf)};
    const char* at = reinterpret_cast<const char*>(pair);
    int handled = 0;

    ASSERT_TRUE(client.ok());
    handled = client.feed(at, split);
    ASSERT_GE(handled, 0) << "split " << split;
    handled += client.feed(at + split, sizeof(pair) - split);
    EXPECT_EQ(handled, 2) << "split " << split;

    EXPECT_EQ(client.response(), 0) << "split " << split;
    EXPECT_GT(client.response(MTL_MSG_TYPE_IF_QUEUE_ID), 0) << "split " << split;
    client.close_instance();
  }
}

/*
 * Register names the AF_XDP interfaces of the instance, so it must only succeed
 * when the XDP program is on every one of them. An interface another request
 * already took over, at a time when the program could not attach, has no
 * program, and handing that one back reports a register the instance cannot
 * use: every later xsk map and filter call on it answers -ENOTSUP.
 */
TEST_F(AbuseTest, RegisterFailsWhenTheInterfaceHasNoXdpProgram) {
  abuse_client& first = new_client();
  abuse_client& second = new_client();

  /* The program cannot attach to kIf, which is what a manager without the
   * rights for it sees. kOtherIf still takes one. */
  xdp->attach_fails_on.push_back(kIf);

  do_register(first, kOtherIf);

  /* This takes kIf over without a program, because a queue needs none. */
  ASSERT_EQ(first.feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, kIf)), 1);
  ASSERT_GT(first.response(MTL_MSG_TYPE_IF_QUEUE_ID), 0);

  ASSERT_EQ(second.feed(register_msg(kIf)), 1);
  EXPECT_EQ(second.response(), -ENODEV)
      << "register reported success for an interface with no XDP program";
  EXPECT_FALSE(second.instance->registered());
}

/*
 * Many clients, many interfaces, everything taken and given back.
 *
 * The case asserts that the counts return to zero. It also runs every
 * allocation path of the instance and the registry several hundred times, which
 * is what a leak checker needs to report a block nothing frees.
 */
TEST_F(AbuseTest, ChurnLeavesNothingBehind) {
  constexpr int kRounds = 50;

  for (unsigned int i = 3; i < 10; i++) netdev->add_if(i, 4, 8);

  for (int round = 0; round < kRounds; round++) {
    for (unsigned int ifindex = 1; ifindex < 10; ifindex++) {
      abuse_client& client = new_client();
      uint16_t lcore = static_cast<uint16_t>(ifindex);

      ASSERT_TRUE(client.ok());
      ASSERT_NO_FATAL_FAILURE(do_register(client, ifindex));

      ASSERT_EQ(client.feed(filter_msg(MTL_MSG_TYPE_ADD_UDP_DP_FILTER, ifindex,
                                       static_cast<uint16_t>(6000 + ifindex))),
                1);
      ASSERT_EQ(client.response(), 0);

      mtl_message_t lcore_get =
          wire_request(MTL_MSG_TYPE_GET_LCORE, sizeof(mtl_lcore_message_t));
      lcore_get.body.lcore_msg.lcore = htons(lcore);
      ASSERT_EQ(client.feed(lcore_get), 1);
      ASSERT_EQ(client.response(), 0);

      ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, ifindex)), 1);
      ASSERT_GT(client.response(MTL_MSG_TYPE_IF_QUEUE_ID), 0);

      mtl_message_t flow = if_msg(MTL_MSG_TYPE_IF_ADD_FLOW, ifindex);
      flow.body.if_msg.queue_id = htons(1);
      flow.body.if_msg.flow_type = htonl(0x02);
      flow.body.if_msg.dst_port = htons(20000);
      ASSERT_EQ(client.feed(flow), 1);
      ASSERT_GT(client.response(MTL_MSG_TYPE_IF_FLOW_ID), 0);

      /* Die while holding all four. */
      clients.clear();

      EXPECT_EQ(lcores.used_count(), 0u) << "round " << round << " if " << ifindex;
      EXPECT_EQ(registry->live_count(), 0u) << "round " << round << " if " << ifindex;
      EXPECT_TRUE(netdev->ifaces[ifindex].rules.empty())
          << "round " << round << " if " << ifindex;
    }
  }
}

/*
 * A record with the right magic, then bytes that are not a record.
 *
 * feed() reports -EBADMSG, the server drops the connection, and nothing the
 * instance holds may outlive it.
 */
TEST_F(AbuseTest, AStreamThatLosesItsBoundaryIsRefusedAndReleasesEverything) {
  abuse_client& client = new_client();
  std::vector<char> junk(MTL_MANAGER_MSG_SIZE, '\xAB');

  do_register(client);

  ASSERT_EQ(
      client.feed(wire_request(MTL_MSG_TYPE_GET_LCORE, sizeof(mtl_lcore_message_t))), 1);
  ASSERT_EQ(client.response(), 0);
  ASSERT_EQ(lcores.used_count(), 1u);

  EXPECT_EQ(client.feed(junk.data(), junk.size()), -EBADMSG);

  client.close_instance();
  EXPECT_EQ(lcores.used_count(), 0u);
  EXPECT_EQ(registry->live_count(), 0u);
}

/*
 * One client that asks and never reads the answers.
 *
 * The server has one thread and one epoll loop, and it writes each answer with
 * a blocking send. So the answers of a client that does not read them fill the
 * socket, the send stops in the kernel, and the loop stops with it: no other
 * instance on the host is served, and no new one can connect. A client does not
 * have to be hostile to do this. One that is stopped, or one that asks from two
 * threads while only one of them reads, is enough.
 *
 * The server must keep serving the others. The only answer it can give a client
 * whose socket stays full is to drop it.
 */
TEST_F(AbuseServerTest, AClientThatNeverReadsDoesNotStopTheServer) {
  /* Enough records to fill the socket both ways. One answer is as large as one
   * request, and the buffer of an AF_UNIX socket holds about 200 kB. */
  constexpr int kFloodRecords = 8000;
  /* Stop when the socket takes nothing more for this long. The server is then
   * either done reading or waiting in send() to this socket, and either way
   * there is no point sending more. */
  constexpr int kStuckTries = 20;
  mtl_message_t msg = heartbeat(1);
  int flood = connect_client();
  int fd = -1;
  int sent = 0;
  int stuck = 0;

  ASSERT_GE(flood, 0);

  for (int i = 0; i < kFloodRecords && stuck < kStuckTries; i++) {
    ssize_t ret = send(flood, &msg, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (ret == static_cast<ssize_t>(MTL_MANAGER_MSG_SIZE)) {
      sent++;
      stuck = 0;
      continue;
    }
    if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      stuck++;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      i--; /* the record did not go, so it is not one of the flood */
      continue;
    }
    break;
  }
  ASSERT_GT(sent, 0);

  /* A second client, which behaves. It must get its answer. */
  fd = connect_client();
  ASSERT_GE(fd, 0);
  wire_set_timeout(fd, 5);

  mtl_message_t good = heartbeat(77);
  ASSERT_EQ(send(fd, &good, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL),
            static_cast<ssize_t>(MTL_MANAGER_MSG_SIZE));

  mtl_message_t reply;
  ASSERT_EQ(wire_read(fd, reply), 0) << "the server stopped serving every other client";
  EXPECT_EQ(wire_type(reply), static_cast<uint32_t>(MTL_MSG_TYPE_HEARTBEAT_ACK));
  EXPECT_EQ(ntohl(reply.body.heartbeat_msg.seq), 77u);
}
