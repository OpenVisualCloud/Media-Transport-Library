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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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
    struct timeval tv = {5, 0};
    setsockopt(fds[1], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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
    struct sockaddr_un addr = unix_addr(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) return -errno;
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      int ret = -errno;
      close(fd);
      return ret;
    }
    conns.push_back(fd);
    return fd;
  }

  /** Close one client before the fixture does. */
  void close_client(int fd) {
    for (int& conn : conns)
      if (conn == fd) conn = -1;
    close(fd);
  }

  /** Descriptor count once it is `want`, or after 5 s, or a negative errno. */
  int settled_fd_count(int want) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    mtl_message_t msg = wire_heartbeat(0);
    int fd = connect_client();

    if (fd < 0) return fd;
    /* Accept is in order, so this answer proves every earlier client accepted. */
    wire_set_timeout(fd, 5);
    send(fd, &msg, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL);
    int ret = wire_read(fd, msg);
    close_client(fd);
    if (ret < 0) return ret;

    int now = open_fd_count();

    while (now != want && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      now = open_fd_count();
    }
    return now;
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

      ASSERT_EQ(client.feed(wire_lcore(MTL_MSG_TYPE_GET_LCORE, lcore)), 1);
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

  ASSERT_EQ(client.feed(wire_lcore(MTL_MSG_TYPE_GET_LCORE, 0)), 1);
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
  mtl_message_t msg = wire_heartbeat(1);
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

  mtl_message_t good = wire_heartbeat(77);
  ASSERT_EQ(send(fd, &good, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL),
            static_cast<ssize_t>(MTL_MANAGER_MSG_SIZE));

  mtl_message_t reply;
  ASSERT_EQ(wire_read(fd, reply), 0) << "the server stopped serving every other client";
  EXPECT_EQ(wire_type(reply), static_cast<uint32_t>(MTL_MSG_TYPE_HEARTBEAT_ACK));
  EXPECT_EQ(ntohl(reply.body.heartbeat_msg.seq), 77u);
}

/* The frame is always one whole record, so body_len must never move the boundary. */
TEST_F(AbuseTest, ABodyLenThatDisagreesWithTheRecordIsNotFollowed) {
  abuse_client& client = new_client();
  const uint32_t lens[] = {0,
                           1,
                           sizeof(mtl_heartbeat_message_t),
                           sizeof(mtl_heartbeat_message_t) + 1,
                           4 * MTL_MANAGER_MSG_SIZE,
                           UINT32_MAX};
  uint32_t seq = 100;

  ASSERT_TRUE(client.ok());
  for (uint32_t len : lens) {
    mtl_message_t msg = wire_request(MTL_MSG_TYPE_HEARTBEAT, len);
    mtl_message_t reply;

    msg.body.heartbeat_msg.seq = htonl(++seq);
    ASSERT_EQ(client.feed(msg), 1) << "body_len " << len;
    ASSERT_EQ(wire_read(client.peer_fd, reply), 0) << "body_len " << len;
    EXPECT_EQ(wire_type(reply), static_cast<uint32_t>(MTL_MSG_TYPE_HEARTBEAT_ACK));
    EXPECT_EQ(ntohl(reply.body.heartbeat_msg.seq), seq) << "body_len " << len;
  }
}

/* A sender that trusts its own body_len puts extra bytes between two records. */
TEST_F(AbuseTest, ARecordLongerThanTheFrameLosesTheBoundary) {
  abuse_client& client = new_client();
  mtl_message_t first = wire_request(MTL_MSG_TYPE_HEARTBEAT, MTL_MANAGER_MSG_SIZE + 64);
  mtl_message_t second = wire_request(MTL_MSG_TYPE_HEARTBEAT, 0);
  std::vector<char> stream;

  do_register(client);
  ASSERT_EQ(client.feed(wire_lcore(MTL_MSG_TYPE_GET_LCORE, 0)), 1);
  ASSERT_EQ(client.response(), 0);

  first.body.heartbeat_msg.seq = htonl(1);
  stream.insert(stream.end(), reinterpret_cast<char*>(&first),
                reinterpret_cast<char*>(&first) + MTL_MANAGER_MSG_SIZE);
  stream.insert(stream.end(), 64, '\xCD');
  stream.insert(stream.end(), reinterpret_cast<char*>(&second),
                reinterpret_cast<char*>(&second) + MTL_MANAGER_MSG_SIZE);

  EXPECT_EQ(client.feed(stream.data(), stream.size()), -EBADMSG);
  EXPECT_EQ(client.response(MTL_MSG_TYPE_HEARTBEAT_ACK), 1);

  client.close_instance();
  EXPECT_EQ(lcores.used_count(), 0u);
  EXPECT_EQ(registry->live_count(), 0u);
}

/* Many records in one call, more answers than the socket holds. */
TEST_F(AbuseTest, OneFeedOfManyRecords) {
  constexpr uint32_t kRecords = 2000;
  abuse_client& client = new_client();
  std::vector<mtl_message_t> stream(kRecords);
  uint32_t acked = 0;

  ASSERT_TRUE(client.ok());
  for (uint32_t i = 0; i < kRecords; i++) {
    stream[i] = wire_request(MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));
    stream[i].body.heartbeat_msg.seq = htonl(i);
  }

  std::thread reader([&client, &acked]() {
    mtl_message_t reply;

    for (uint32_t i = 0; i < kRecords && wire_read(client.peer_fd, reply) == 0; i++)
      if (ntohl(reply.body.heartbeat_msg.seq) == i) acked++;
  });
  int handled = client.feed(stream.data(), stream.size() * MTL_MANAGER_MSG_SIZE);
  reader.join();

  EXPECT_EQ(handled, static_cast<int>(kRecords));
  EXPECT_EQ(acked, kRecords);
}

TEST_F(AbuseTest, AByteAtATimeFeedOfManyRecords) {
  constexpr uint32_t kRecords = 200;
  abuse_client& client = new_client();
  int handled = 0;

  ASSERT_TRUE(client.ok());
  for (uint32_t i = 0; i < kRecords; i++) {
    mtl_message_t msg =
        wire_request(MTL_MSG_TYPE_HEARTBEAT, sizeof(mtl_heartbeat_message_t));
    const char* at = reinterpret_cast<const char*>(&msg);

    msg.body.heartbeat_msg.seq = htonl(i);
    for (size_t b = 0; b < MTL_MANAGER_MSG_SIZE; b++) {
      int ret = client.feed(at + b, 1);
      ASSERT_EQ(ret, b + 1 == MTL_MANAGER_MSG_SIZE ? 1 : 0)
          << "record " << i << " byte " << b;
      handled += ret;
    }
  }
  EXPECT_EQ(handled, static_cast<int>(kRecords));

  for (uint32_t i = 0; i < kRecords; i++) {
    mtl_message_t reply;

    ASSERT_EQ(wire_read(client.peer_fd, reply), 0) << "record " << i;
    EXPECT_EQ(ntohl(reply.body.heartbeat_msg.seq), i);
  }
}

TEST_F(AbuseTest, LcoreIdsAtAndPastTheTableBound) {
  abuse_client& client = new_client();

  do_register(client);

  ASSERT_EQ(client.feed(wire_lcore(MTL_MSG_TYPE_GET_LCORE, MTL_MANAGER_MAX_LCORE - 1)),
            1);
  EXPECT_EQ(client.response(), 0);
  for (uint16_t id : std::initializer_list<uint16_t>{MTL_MANAGER_MAX_LCORE, UINT16_MAX}) {
    ASSERT_EQ(client.feed(wire_lcore(MTL_MSG_TYPE_GET_LCORE, id)), 1);
    EXPECT_EQ(client.response(), -EINVAL) << "get " << id;
    ASSERT_EQ(client.feed(wire_lcore(MTL_MSG_TYPE_PUT_LCORE, id)), 1);
    EXPECT_EQ(client.response(), -EINVAL) << "put " << id;
  }
  EXPECT_EQ(lcores.used_count(), 1u);

  client.close_instance();
  EXPECT_EQ(lcores.used_count(), 0u);
}

/* No request may take or build an interface for an ifindex the device lacks. */
TEST_F(AbuseTest, IfindexesThatNameNothing) {
  abuse_client& client = new_client();
  mtl_message_t reg = register_msg(kIf);

  reg.body.register_msg.num_if = htons(0);
  ASSERT_TRUE(client.ok());
  ASSERT_EQ(client.feed(reg), 1);
  ASSERT_EQ(client.response(), 0);

  for (unsigned int ifindex : {0u, 99u, UINT32_MAX}) {
    ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, ifindex)), 1);
    EXPECT_EQ(client.response(MTL_MSG_TYPE_IF_QUEUE_ID), -ENODEV) << ifindex;
    ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_ADD_FLOW, ifindex)), 1);
    EXPECT_EQ(client.response(MTL_MSG_TYPE_IF_FLOW_ID), -ENODEV) << ifindex;
    ASSERT_EQ(client.feed(filter_msg(MTL_MSG_TYPE_ADD_UDP_DP_FILTER, ifindex, 1)), 1);
    EXPECT_EQ(client.response(), -ENODEV) << ifindex;
    ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_PUT_QUEUE, ifindex)), 1);
    EXPECT_EQ(client.response(), -EINVAL) << ifindex;
    ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_DEL_FLOW, ifindex)), 1);
    EXPECT_EQ(client.response(), -EINVAL) << ifindex;
    ASSERT_EQ(client.feed(filter_msg(MTL_MSG_TYPE_DEL_UDP_DP_FILTER, ifindex, 1)), 1);
    EXPECT_EQ(client.response(), -EINVAL) << ifindex;
    ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_XSK_MAP_FD, ifindex)), 1);
    EXPECT_EQ(client.xsk_map_fd(), -ENOTSUP) << ifindex;
  }

  EXPECT_EQ(registry->live_count(), 0u);
  EXPECT_EQ(xdp->attach_calls, 0);
  EXPECT_EQ(netdev->insert_calls, 0);
}

TEST_F(AbuseTest, UnheldQueueAndFlowIdsReachNoDevice) {
  abuse_client& client = new_client();

  do_register(client);
  ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, kIf)), 1);
  int queue = client.response(MTL_MSG_TYPE_IF_QUEUE_ID);
  ASSERT_GT(queue, 0);

  mtl_message_t unheld = if_msg(MTL_MSG_TYPE_IF_PUT_QUEUE, kIf);
  unheld.body.if_msg.queue_id = htons(static_cast<uint16_t>(queue + 1));
  ASSERT_EQ(client.feed(unheld), 1);
  EXPECT_EQ(client.response(), -EINVAL);

  mtl_message_t del = if_msg(MTL_MSG_TYPE_IF_DEL_FLOW, kIf);
  del.body.if_msg.flow_id = htonl(1);
  ASSERT_EQ(client.feed(del), 1);
  EXPECT_EQ(client.response(), -EINVAL);
  EXPECT_EQ(netdev->delete_calls, 1) << "only the foreign rule, at the take-over";

  mtl_message_t put = if_msg(MTL_MSG_TYPE_IF_PUT_QUEUE, kIf);
  put.body.if_msg.queue_id = htons(static_cast<uint16_t>(queue));
  ASSERT_EQ(client.feed(put), 1);
  EXPECT_EQ(client.response(), 0);
  ASSERT_EQ(client.feed(put), 1);
  EXPECT_EQ(client.response(), -EINVAL);
}

/* The hostname field fills all its bytes and has no zero. */
TEST_F(AbuseTest, ARegisterWithBogusIdentity) {
  abuse_client& client = new_client();
  mtl_message_t reg = register_msg(kIf);

  reg.body.register_msg.pid = static_cast<pid_t>(htonl(UINT32_MAX));
  reg.body.register_msg.uid = static_cast<uid_t>(htonl(0x80000000u));
  std::memset(reg.body.register_msg.hostname, 'h',
              sizeof(reg.body.register_msg.hostname));

  ASSERT_TRUE(client.ok());
  ASSERT_EQ(client.feed(reg), 1);
  EXPECT_EQ(client.response(), 0);
  EXPECT_EQ(client.instance->get_hostname(),
            std::string(sizeof(reg.body.register_msg.hostname), 'h'));
}

TEST_F(AbuseTest, MessagesAfterAFailedRegisterAreRefused) {
  abuse_client& client = new_client();
  mtl_message_t too_many = register_msg(kIf);
  mtl_message_t lcore_get =
      wire_request(MTL_MSG_TYPE_GET_LCORE, sizeof(mtl_lcore_message_t));

  too_many.body.register_msg.num_if = htons(MTL_MANAGER_MAX_IF + 1);
  ASSERT_TRUE(client.ok());

  ASSERT_EQ(client.feed(too_many), 1);
  EXPECT_EQ(client.response(), -EINVAL);
  ASSERT_EQ(client.feed(lcore_get), 1);
  EXPECT_EQ(client.response(), -EPERM);

  ASSERT_EQ(client.feed(register_msg(99)), 1);
  EXPECT_EQ(client.response(), -ENODEV);
  ASSERT_EQ(client.feed(lcore_get), 1);
  EXPECT_EQ(client.response(), -EPERM);
  EXPECT_EQ(lcores.used_count(), 0u);
}

/* The first interface of a register that fails on the second must still go back. */
TEST_F(AbuseTest, AHalfDoneRegisterGivesBackTheInterfaceItTook) {
  abuse_client& client = new_client();
  mtl_message_t reg = register_msg(kIf);

  reg.body.register_msg.num_if = htons(2);
  reg.body.register_msg.ifindex[1] = htonl(99);
  ASSERT_TRUE(client.ok());
  ASSERT_EQ(client.feed(reg), 1);
  EXPECT_EQ(client.response(), -ENODEV);

  client.close_instance();
  EXPECT_EQ(registry->live_count(), 0u);
  EXPECT_EQ(xdp->detach_calls, xdp->attach_calls);
  EXPECT_FALSE(xdp->attached);
}

/* A second register widens the first; death must release on every interface. */
TEST_F(AbuseTest, DeathWithResourcesOnSeveralInterfaces) {
  constexpr unsigned int kIfs = 4;
  abuse_client& client = new_client();
  mtl_message_t wide = register_msg(kIf);

  netdev->add_if(3, 4, 8);
  netdev->add_if(4, 4, 8);
  wide.body.register_msg.num_if = htons(kIfs);
  for (unsigned int i = 0; i < kIfs; i++)
    wide.body.register_msg.ifindex[i] = htonl(i + 1);

  do_register(client);
  ASSERT_EQ(client.feed(wide), 1);
  ASSERT_EQ(client.response(), 0);

  for (unsigned int ifindex = 1; ifindex <= kIfs; ifindex++) {
    ASSERT_EQ(client.feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, ifindex)), 1);
    int queue = client.response(MTL_MSG_TYPE_IF_QUEUE_ID);
    ASSERT_GT(queue, 0);

    mtl_message_t flow = if_msg(MTL_MSG_TYPE_IF_ADD_FLOW, ifindex);
    flow.body.if_msg.queue_id = htons(static_cast<uint16_t>(queue));
    flow.body.if_msg.flow_type = htonl(0x02);
    ASSERT_EQ(client.feed(flow), 1);
    ASSERT_GT(client.response(MTL_MSG_TYPE_IF_FLOW_ID), 0);

    for (uint16_t port : std::initializer_list<uint16_t>{0, UINT16_MAX}) {
      ASSERT_EQ(client.feed(filter_msg(MTL_MSG_TYPE_ADD_UDP_DP_FILTER, ifindex, port)),
                1);
      ASSERT_EQ(client.response(), 0);
    }
  }
  for (uint16_t id : std::initializer_list<uint16_t>{0, MTL_MANAGER_MAX_LCORE - 1}) {
    ASSERT_EQ(client.feed(wire_lcore(MTL_MSG_TYPE_GET_LCORE, id)), 1);
    ASSERT_EQ(client.response(), 0);
  }
  ASSERT_EQ(registry->live_count(), kIfs);

  client.close_instance();

  EXPECT_EQ(lcores.used_count(), 0u);
  EXPECT_EQ(registry->live_count(), 0u);
  EXPECT_EQ(xdp->detach_calls, xdp->attach_calls);
  EXPECT_EQ(filter_calls(0, false), kIfs);
  EXPECT_EQ(filter_calls(UINT16_MAX, false), kIfs);
  for (unsigned int ifindex = 1; ifindex <= kIfs; ifindex++)
    EXPECT_TRUE(netdev->ifaces[ifindex].rules.empty()) << "if " << ifindex;
}

/* The registry entry expires between the two halves of a record naming it. */
TEST_F(AbuseTest, TheLastHolderDiesWhileAnotherIsMidRecord) {
  abuse_client& holder = new_client();
  abuse_client& waiter = new_client();
  mtl_message_t reg = register_msg(kIf);
  mtl_message_t get = if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, kIf);
  const char* at = reinterpret_cast<const char*>(&get);
  const size_t half = MTL_MANAGER_MSG_SIZE / 2;

  do_register(holder);
  ASSERT_EQ(holder.feed(if_msg(MTL_MSG_TYPE_IF_GET_QUEUE, kIf)), 1);
  ASSERT_EQ(holder.response(MTL_MSG_TYPE_IF_QUEUE_ID), 1);

  reg.body.register_msg.num_if = htons(0);
  ASSERT_TRUE(waiter.ok());
  ASSERT_EQ(waiter.feed(reg), 1);
  ASSERT_EQ(waiter.response(), 0);
  ASSERT_EQ(waiter.feed(at, half), 0);

  holder.close_instance();
  ASSERT_EQ(registry->live_count(), 0u);

  ASSERT_EQ(waiter.feed(at + half, MTL_MANAGER_MSG_SIZE - half), 1);
  EXPECT_EQ(waiter.response(MTL_MSG_TYPE_IF_QUEUE_ID), 1)
      << "the queue of the dead holder must be free on the new interface";
  EXPECT_EQ(registry->live_count(), 1u);
  EXPECT_EQ(xdp->attach_calls, 2);
}

TEST_F(AbuseServerTest, AFullServerServesAgainOnceClientsLeave) {
  const size_t kLimit = mtlm_server_config().max_clients;
  std::vector<int> full;
  mtl_message_t reply;
  int baseline = open_fd_count();

  for (size_t i = 0; i < kLimit; i++) {
    mtl_message_t msg = wire_heartbeat(static_cast<uint32_t>(i));
    int fd = connect_client();

    ASSERT_GE(fd, 0) << "client " << i;
    wire_set_timeout(fd, 5);
    ASSERT_EQ(send(fd, &msg, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL),
              static_cast<ssize_t>(MTL_MANAGER_MSG_SIZE));
    ASSERT_EQ(wire_read(fd, reply), 0) << "client " << i;
    full.push_back(fd);
  }

  int extra = connect_client();
  ASSERT_GE(extra, 0);
  wire_set_timeout(extra, 5);
  mtl_message_t msg = wire_heartbeat(1);
  send(extra, &msg, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL);
  EXPECT_EQ(wire_read(extra, reply), -ECONNRESET) << "a client past the limit was served";
  close_client(extra);

  for (int fd : full) close_client(fd);

  /* The server sees the hang-ups in its own time, so retry until it has. */
  bool served = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!served && std::chrono::steady_clock::now() < deadline) {
    int fd = connect_client();

    ASSERT_GE(fd, 0);
    wire_set_timeout(fd, 1);
    msg = wire_heartbeat(77);
    served = send(fd, &msg, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL) ==
                 static_cast<ssize_t>(MTL_MANAGER_MSG_SIZE) &&
             wire_read(fd, reply) == 0 && ntohl(reply.body.heartbeat_msg.seq) == 77u;
    close_client(fd);
    if (!served) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(served) << "the server stayed full after every client left";
  EXPECT_EQ(settled_fd_count(baseline), baseline);
}

/* Close at once, close mid-record, and close with an answer not read. */
TEST_F(AbuseServerTest, ConnectAndCloseCyclesLeaveNoDescriptorBehind) {
  constexpr int kCycles = 600;
  mtl_message_t msg = wire_heartbeat(5);
  int baseline = open_fd_count();

  ASSERT_GT(baseline, 0);
  for (int i = 0; i < kCycles; i++) {
    int fd = connect_client();

    ASSERT_GE(fd, 0) << "cycle " << i;
    if (i % 3 == 1) send(fd, &msg, MTL_MANAGER_MSG_SIZE / 2, MSG_NOSIGNAL);
    if (i % 3 == 2) send(fd, &msg, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL);
    close_client(fd);
  }

  EXPECT_EQ(settled_fd_count(baseline), baseline);

  int fd = connect_client();
  mtl_message_t reply;
  ASSERT_GE(fd, 0);
  wire_set_timeout(fd, 5);
  ASSERT_EQ(send(fd, &msg, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL),
            static_cast<ssize_t>(MTL_MANAGER_MSG_SIZE));
  EXPECT_EQ(wire_read(fd, reply), 0);
}

/* On the real device an ifindex no host has stops at if_indextoname. */
TEST_F(AbuseServerTest, IfindexesNoHostHasLeaveNoDescriptorBehind) {
  constexpr int kRounds = 50;
  mtl_message_t reg = wire_request(MTL_MSG_TYPE_REGISTER, sizeof(mtl_register_message_t));
  mtl_message_t reply;
  int fd = connect_client();

  ASSERT_GE(fd, 0);
  wire_set_timeout(fd, 5);
  ASSERT_EQ(send(fd, &reg, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL),
            static_cast<ssize_t>(MTL_MANAGER_MSG_SIZE));
  ASSERT_EQ(wire_read(fd, reply), 0);
  ASSERT_EQ(wire_response(reply), 0);

  int baseline = open_fd_count();
  for (int round = 0; round < kRounds; round++) {
    for (unsigned int ifindex : {0u, 0x7FFFFFFFu, UINT32_MAX}) {
      mtl_message_t get =
          wire_request(MTL_MSG_TYPE_IF_GET_QUEUE, sizeof(mtl_if_message_t));

      get.body.if_msg.ifindex = htonl(ifindex);
      ASSERT_EQ(send(fd, &get, MTL_MANAGER_MSG_SIZE, MSG_NOSIGNAL),
                static_cast<ssize_t>(MTL_MANAGER_MSG_SIZE));
      ASSERT_EQ(wire_read(fd, reply), 0);
      ASSERT_EQ(wire_type(reply), static_cast<uint32_t>(MTL_MSG_TYPE_IF_QUEUE_ID));
      ASSERT_EQ(wire_response(reply), -ENODEV) << "ifindex " << ifindex;
    }
  }
  EXPECT_EQ(open_fd_count(), baseline);
}
