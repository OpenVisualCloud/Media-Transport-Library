/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * The server: where it binds, who may reach the socket, and what it leaves
 * behind. Every case uses a path below /tmp, so none of them needs root.
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "mtlm_api.h"
#include "mtlm_server.hpp"

namespace {

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mtlm-server-test-XXXXXX";

    ASSERT_NE(mkdtemp(pattern), nullptr);
    dir = pattern;
    path = dir + "/mtl_manager.sock";
  }

  void TearDown() override {
    unlink(path.c_str());
    rmdir(dir.c_str());
  }

  mtlm_server_config config() {
    mtlm_server_config cfg;

    cfg.sock_path = path;
    return cfg;
  }

  std::string dir;
  std::string path;
};

/** Run a server in a thread and stop it again. */
class server_thread {
 public:
  explicit server_thread(mtlm_server& server) : server(server) {
    worker = std::thread([this]() { this->server.run(); });
  }

  ~server_thread() {
    server.stop();
    if (worker.joinable()) worker.join();
  }

 private:
  mtlm_server& server;
  std::thread worker;
};

} /* namespace */

TEST_F(ServerTest, SetupBindsThePathItWasGiven) {
  mtlm_server server(config());

  ASSERT_EQ(server.setup(), 0);
  EXPECT_EQ(server.sock_path(), path);

  struct stat st = {};
  ASSERT_EQ(stat(path.c_str(), &st), 0);
  EXPECT_TRUE(S_ISSOCK(st.st_mode));
}

TEST_F(ServerTest, APerUserSocketKeepsOtherUsersOut) {
  mtlm_server server(config());

  ASSERT_EQ(server.setup(), 0);

  struct stat st = {};
  ASSERT_EQ(stat(path.c_str(), &st), 0);
  /* The old code made the socket 0777, so any local user could drive the
   * manager. A path outside the system directory belongs to one user. */
  EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST_F(ServerTest, AnAskedForModeWins) {
  mtlm_server_config cfg = config();

  cfg.socket_mode = 0660;
  mtlm_server server(cfg);
  ASSERT_EQ(server.setup(), 0);

  struct stat st = {};
  ASSERT_EQ(stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0660u);
}

TEST_F(ServerTest, AnUnknownGroupStopsTheStart) {
  mtlm_server_config cfg = config();

  cfg.socket_group = "no-such-group-for-the-mtl-test";
  mtlm_server server(cfg);
  EXPECT_EQ(server.setup(), -ENOENT);
}

TEST_F(ServerTest, TheSocketFileGoesAwayWithTheServer) {
  {
    mtlm_server server(config());
    ASSERT_EQ(server.setup(), 0);
    ASSERT_EQ(access(path.c_str(), F_OK), 0);
  }

  EXPECT_NE(access(path.c_str(), F_OK), 0);
}

TEST_F(ServerTest, ASecondServerRefusesTheSamePath) {
  mtlm_server first(config());

  ASSERT_EQ(first.setup(), 0);
  server_thread running(first);

  mtlm_server second(config());
  /* Binding over a live manager used to cut every client off it. */
  EXPECT_EQ(second.setup(), -EADDRINUSE);

  /* The refusal must leave the socket of the first server in place. */
  EXPECT_EQ(access(path.c_str(), F_OK), 0);
}

TEST_F(ServerTest, ASocketFileWithNoManagerIsReplaced) {
  {
    mtlm_server crashed(config());
    ASSERT_EQ(crashed.setup(), 0);
    /* Leave the file behind, as a manager killed with SIGKILL would. */
    ASSERT_EQ(link(path.c_str(), (path + ".keep").c_str()), 0);
  }
  ASSERT_EQ(rename((path + ".keep").c_str(), path.c_str()), 0);
  ASSERT_EQ(access(path.c_str(), F_OK), 0);

  mtlm_server fresh(config());
  EXPECT_EQ(fresh.setup(), 0);
}

TEST_F(ServerTest, APathTooLongForAnAddressIsRefused) {
  mtlm_server_config cfg = config();

  cfg.sock_path = "/tmp/" + std::string(MTLM_SOCK_PATH_MAX, 'x') + "/sock";
  mtlm_server server(cfg);
  EXPECT_EQ(server.setup(), -ENAMETOOLONG);
}

TEST_F(ServerTest, ADirectoryItCannotMakeIsRefused) {
  mtlm_server_config cfg = config();

  /* /proc is not writable, so the directory cannot be made. */
  cfg.sock_path = "/proc/mtlm-test/mtl_manager.sock";
  mtlm_server server(cfg);
  EXPECT_LT(server.setup(), 0);
}

TEST_F(ServerTest, AClientGetsServed) {
  mtlm_server server(config());

  ASSERT_EQ(server.setup(), 0);
  server_thread running(server);

  mtlm_client* client = mtlm_client_create(path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);
  EXPECT_EQ(std::string(mtlm_client_sock_path(client)), path);

  uint32_t acked = 0;
  EXPECT_EQ(mtlm_heartbeat(client, 7, &acked), 0);
  EXPECT_EQ(acked, 7u);

  mtlm_client_destroy(client);
}

TEST_F(ServerTest, AHeartbeatSequenceNumberIsNotReadAsAnError) {
  mtlm_server server(config());

  ASSERT_EQ(server.setup(), 0);
  server_thread running(server);

  mtlm_client* client = mtlm_client_create(path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);

  /* The ack carries the sequence number where every other reply carries a
   * response code, in the same bytes. A number with the top bit set must
   * therefore not come back as a negative errno. */
  for (uint32_t seq : {0x80000000u, 0xffffffffu, 0xdeadbeefu}) {
    uint32_t acked = 0;

    EXPECT_EQ(mtlm_heartbeat(client, seq, &acked), 0) << "seq " << seq;
    EXPECT_EQ(acked, seq);
  }

  mtlm_client_destroy(client);
}

TEST_F(ServerTest, TheClientLimitIsHeld) {
  mtlm_server_config cfg = config();

  cfg.max_clients = 2;
  mtlm_server server(cfg);
  ASSERT_EQ(server.setup(), 0);
  server_thread running(server);

  std::vector<mtlm_client*> clients;
  for (int i = 0; i < 2; i++) {
    mtlm_client* client = mtlm_client_create(path.c_str());
    ASSERT_NE(client, nullptr) << "client " << i << ": " << strerror(errno);
    ASSERT_EQ(mtlm_heartbeat(client, 1, nullptr), 0);
    clients.push_back(client);
  }

  /* The connect itself works, because listen() takes it. The manager then
   * closes it, so the first request fails. Without the limit one client could
   * open sockets until the process ran out of descriptors. */
  mtlm_client* extra = mtlm_client_create(path.c_str());
  if (extra != nullptr) {
    EXPECT_LT(mtlm_heartbeat(extra, 1, nullptr), 0);
    mtlm_client_destroy(extra);
  }

  for (mtlm_client* client : clients) mtlm_client_destroy(client);
}

TEST_F(ServerTest, ANonRegisteredRequestIsRefusedNotIgnored) {
  mtlm_server server(config());

  ASSERT_EQ(server.setup(), 0);
  server_thread running(server);

  mtlm_client* client = mtlm_client_create(path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);

  EXPECT_EQ(mtlm_lcore_get(client, 3), -EPERM);

  mtlm_client_destroy(client);
}

TEST_F(ServerTest, RunWithNoSetupReportsTheMistake) {
  mtlm_server server(config());

  EXPECT_EQ(server.run(), -EINVAL);
}
