/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * Integration test of the MtlManager client API.
 *
 * Each case starts a real MtlManager process on a socket path below /tmp and
 * drives it through mtlm_api.h only, the same way the MTL library does. It
 * needs no NIC, no hugepage and no root, so it runs on any host and in a
 * container.
 *
 * The manager binary comes from $MTL_MANAGER_BIN, else from the PATH. Set the
 * variable to test a build that is not installed:
 *
 *   MTL_MANAGER_BIN=$PWD/build/manager/MtlManager \
 *     ./build/tests/MtlManagerApiTest
 *
 * A case that asks for a device operation runs against the loopback interface,
 * which has no combined channel and no flow rule table. Such a case checks the
 * error the manager reports, not a queue id. A test that needs a real NIC
 * belongs in KahawaiTest.
 */

#include <gtest/gtest.h>
#include <mtl/mtlm_api.h>
#include <net/if.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/** Path of the manager binary to start. */
std::string manager_binary() {
  const char* from_env = getenv("MTL_MANAGER_BIN");

  if (from_env != nullptr && from_env[0] != '\0') return from_env;
  return "MtlManager";
}

/**
 * One MtlManager process.
 *
 * The constructor waits until the manager answers a connect, so a case that
 * follows knows the socket is there. The destructor asks it to stop with
 * SIGTERM, the signal systemd uses, and waits for it.
 */
class manager_process {
 public:
  manager_process(const std::string& sock_path, const char* log_level = "error")
      : sock_path(sock_path) {
    std::string binary = manager_binary();

    pid = fork();
    if (pid < 0) {
      /* A constructor cannot use ASSERT_*, so it reports the failure and the
       * first call of the case fails on the client it does not get. */
      ADD_FAILURE() << "fork: " << strerror(errno);
      return;
    }
    if (pid == 0) {
      /* The child. It must never return into the test body. */
      const char* args[] = {binary.c_str(), "--sock-path", sock_path.c_str(),
                            "--log-level",  log_level,     nullptr};
      execvp(binary.c_str(), const_cast<char**>(args));
      _exit(127);
    }

    if (!wait_until_alive())
      ADD_FAILURE() << "No manager on " << sock_path
                    << ". Set MTL_MANAGER_BIN to the binary.";
  }

  ~manager_process() {
    stop();
  }

  manager_process(const manager_process&) = delete;
  manager_process& operator=(const manager_process&) = delete;

  /** Stop the manager and return its exit status, or -1 when it was gone. */
  int stop() {
    int status = 0;

    if (pid <= 0) return -1;
    kill(pid, SIGTERM);
    if (waitpid(pid, &status, 0) < 0) status = -1;
    pid = -1;
    return status;
  }

  /** Whether the process is still there. */
  bool running() const {
    return pid > 0 && kill(pid, 0) == 0;
  }

 private:
  bool wait_until_alive() {
    /* 5 seconds in 10 ms steps. A manager binds in a few milliseconds, so this
     * limit only stops a test run from hanging on a broken build. */
    for (int i = 0; i < 500; i++) {
      if (mtlm_manager_alive(sock_path.c_str())) return true;

      int status = 0;
      if (waitpid(pid, &status, WNOHANG) == pid) {
        pid = -1;
        return false;
      }
      usleep(10 * 1000);
    }
    return false;
  }

  std::string sock_path;
  pid_t pid = -1;
};

class ManagerApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mtlm-api-test-XXXXXX";

    ASSERT_NE(mkdtemp(pattern), nullptr) << strerror(errno);
    dir = pattern;
    sock_path = dir + "/mtl_manager.sock";
  }

  void TearDown() override {
    unlink(sock_path.c_str());
    rmdir(dir.c_str());
  }

  /** A connected and registered client with no interface. */
  mtlm_client* registered_client() {
    mtlm_client* client = mtlm_client_create(sock_path.c_str());
    struct mtlm_register_args args;

    if (client == nullptr) return nullptr;

    memset(&args, 0, sizeof(args));
    args.uid = -1;
    if (mtlm_register(client, &args) < 0) {
      mtlm_client_destroy(client);
      return nullptr;
    }
    return client;
  }

  std::string dir;
  std::string sock_path;
};

} /* namespace */

/*
 * The socket path, which every other case depends on.
 */

TEST(ManagerApiSockPath, TheEnvironmentWins) {
  const char* old = getenv("MTL_MANAGER_SOCK_PATH");
  std::string saved = old != nullptr ? old : "";
  char buf[MTLM_SOCK_PATH_MAX];

  setenv("MTL_MANAGER_SOCK_PATH", "/tmp/from-the-environment.sock", 1);
  ASSERT_EQ(mtlm_sock_path_resolve(buf, sizeof(buf)), 0);
  EXPECT_STREQ(buf, "/tmp/from-the-environment.sock");
  EXPECT_STREQ(mtlm_sock_path(), "/tmp/from-the-environment.sock");

  /* With the variable set there is one candidate, so a client cannot fall back
   * to a manager the operator did not name. */
  EXPECT_EQ(mtlm_sock_path_candidate(0, buf, sizeof(buf)), 0);
  EXPECT_STREQ(buf, "/tmp/from-the-environment.sock");
  EXPECT_EQ(mtlm_sock_path_candidate(1, buf, sizeof(buf)), -ENOENT);

  if (saved.empty())
    unsetenv("MTL_MANAGER_SOCK_PATH");
  else
    setenv("MTL_MANAGER_SOCK_PATH", saved.c_str(), 1);
}

TEST(ManagerApiSockPath, WithNoEnvironmentThereAreTwoCandidates) {
  const char* old = getenv("MTL_MANAGER_SOCK_PATH");
  std::string saved = old != nullptr ? old : "";
  char first[MTLM_SOCK_PATH_MAX];
  char second[MTLM_SOCK_PATH_MAX];

  unsetenv("MTL_MANAGER_SOCK_PATH");
  ASSERT_EQ(mtlm_sock_path_candidate(0, first, sizeof(first)), 0);
  ASSERT_EQ(mtlm_sock_path_candidate(1, second, sizeof(second)), 0);
  EXPECT_EQ(mtlm_sock_path_candidate(MTLM_SOCK_PATH_CANDIDATES, first, sizeof(first)),
            -ENOENT);
  EXPECT_STRNE(first, second);

  /* A client that runs without root must look at the per-user path first, so
   * it does not need the system directory at all. */
  if (geteuid() != 0) {
    EXPECT_FALSE(mtlm_sock_path_is_system(first));
    EXPECT_TRUE(mtlm_sock_path_is_system(second));
  } else {
    EXPECT_TRUE(mtlm_sock_path_is_system(first));
  }

  if (!saved.empty()) setenv("MTL_MANAGER_SOCK_PATH", saved.c_str(), 1);
}

TEST(ManagerApiSockPath, NoManagerIsNotAnError) {
  EXPECT_FALSE(mtlm_manager_alive("/tmp/there-is-no-manager-here.sock"));
  EXPECT_EQ(mtlm_client_create("/tmp/there-is-no-manager-here.sock"), nullptr);
}

TEST(ManagerApiSockPath, TheApiAcceptsANullClient) {
  /* A caller that did not check mtlm_client_create() must not crash the
   * process before it can report the failure. */
  mtlm_client_destroy(nullptr);
  EXPECT_EQ(mtlm_client_fd(nullptr), -1);
  EXPECT_EQ(mtlm_client_sock_path(nullptr), nullptr);
  EXPECT_EQ(mtlm_heartbeat(nullptr, 1, nullptr), -EINVAL);
  EXPECT_EQ(mtlm_lcore_get(nullptr, 1), -EINVAL);
  EXPECT_EQ(mtlm_queue_get(nullptr, 1), -EINVAL);
  EXPECT_EQ(mtlm_flow_del(nullptr, 1, 1), -EINVAL);
}

TEST(ManagerApiSockPath, EveryReturnValueHasText) {
  EXPECT_STRNE(mtlm_strerror(0), nullptr);
  EXPECT_STRNE(mtlm_strerror(-EPERM), nullptr);
  EXPECT_STRNE(mtlm_strerror(-ENODEV), nullptr);
  /* A positive value is a resource id, not a failure. */
  EXPECT_STRNE(mtlm_strerror(7), nullptr);
}

TEST(ManagerApiVersion, TheProtocolVersionIsTwoNumbers) {
  std::string version = mtlm_proto_version();

  EXPECT_NE(version.find('.'), std::string::npos) << version;
}

/*
 * The connection.
 */

TEST_F(ManagerApiTest, AManagerBindsThePathItWasGiven) {
  manager_process manager(sock_path);
  struct stat st = {};

  ASSERT_EQ(stat(sock_path.c_str(), &st), 0) << strerror(errno);
  EXPECT_TRUE(S_ISSOCK(st.st_mode));
  /* A path of one user keeps every other user out. Running the manager needs
   * no root, so the socket must not be open to everyone. */
  EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST_F(ManagerApiTest, TheSocketGoesAwayWithTheManager) {
  {
    manager_process manager(sock_path);
    ASSERT_EQ(access(sock_path.c_str(), F_OK), 0);
  }

  /* A file left behind makes the next mtlm_manager_alive() lie. */
  EXPECT_NE(access(sock_path.c_str(), F_OK), 0);
  EXPECT_FALSE(mtlm_manager_alive(sock_path.c_str()));
}

TEST_F(ManagerApiTest, SigtermStopsTheManagerCleanly) {
  manager_process manager(sock_path);

  int status = manager.stop();
  ASSERT_TRUE(WIFEXITED(status)) << "the manager did not exit on SIGTERM";
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_F(ManagerApiTest, AClientConnectsAndReportsItsPath) {
  manager_process manager(sock_path);

  mtlm_client* client = mtlm_client_create(sock_path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);
  EXPECT_GE(mtlm_client_fd(client), 0);
  EXPECT_STREQ(mtlm_client_sock_path(client), sock_path.c_str());

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, ANullPathWalksTheCandidates) {
  const char* old = getenv("MTL_MANAGER_SOCK_PATH");
  std::string saved = old != nullptr ? old : "";

  setenv("MTL_MANAGER_SOCK_PATH", sock_path.c_str(), 1);
  {
    manager_process manager(sock_path);

    /* This is what the MTL library does: it names no path. */
    mtlm_client* client = mtlm_client_create(nullptr);
    ASSERT_NE(client, nullptr) << strerror(errno);
    EXPECT_STREQ(mtlm_client_sock_path(client), sock_path.c_str());
    EXPECT_TRUE(mtlm_manager_alive(nullptr));
    mtlm_client_destroy(client);
  }

  if (saved.empty())
    unsetenv("MTL_MANAGER_SOCK_PATH");
  else
    setenv("MTL_MANAGER_SOCK_PATH", saved.c_str(), 1);
}

/*
 * Register, and the rule that every other operation needs it.
 */

TEST_F(ManagerApiTest, RegisterWithNoInterfaceSucceeds) {
  manager_process manager(sock_path);
  struct mtlm_register_args args;

  mtlm_client* client = mtlm_client_create(sock_path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);

  memset(&args, 0, sizeof(args));
  args.uid = -1;
  /* An instance that uses the DPDK backend needs lcores only, so it names no
   * interface. The old protocol had no way to say that. */
  EXPECT_EQ(mtlm_register(client, &args), 0);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, RegisterNamesTheFieldsItself) {
  manager_process manager(sock_path);
  struct mtlm_register_args args;

  mtlm_client* client = mtlm_client_create(sock_path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);

  memset(&args, 0, sizeof(args));
  /* 0 and -1 ask the API for getpid() and getuid(), and NULL for the host
   * name, so a caller cannot report a wrong identity by leaving a field out. */
  args.pid = 0;
  args.uid = -1;
  args.hostname = nullptr;
  EXPECT_EQ(mtlm_register(client, &args), 0);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, RegisterRefusesAnUnknownInterface) {
  manager_process manager(sock_path);
  struct mtlm_register_args args;
  unsigned int ifindex = 0xfffffff;

  mtlm_client* client = mtlm_client_create(sock_path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);

  memset(&args, 0, sizeof(args));
  args.uid = -1;
  args.ifindex = &ifindex;
  args.num_if = 1;
  EXPECT_EQ(mtlm_register(client, &args), -ENODEV);

  /* The connection must stay usable, so the client can report the failure and
   * try again with the right index. */
  EXPECT_EQ(mtlm_heartbeat(client, 1, nullptr), 0);
  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, RegisterRefusesTooManyInterfaces) {
  manager_process manager(sock_path);
  struct mtlm_register_args args;
  std::vector<unsigned int> indexes(MTL_MANAGER_MAX_IF + 1, 1);

  mtlm_client* client = mtlm_client_create(sock_path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);

  memset(&args, 0, sizeof(args));
  args.uid = -1;
  args.ifindex = indexes.data();
  args.num_if = static_cast<uint16_t>(indexes.size());
  /* The API stops this one, so the oversized record never reaches the wire. */
  EXPECT_EQ(mtlm_register(client, &args), -EINVAL);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, EveryOperationNeedsARegisteredClient) {
  manager_process manager(sock_path);
  struct mtlm_flow flow;

  mtlm_client* client = mtlm_client_create(sock_path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);

  memset(&flow, 0, sizeof(flow));
  flow.ifindex = 1;
  flow.queue_id = 1;
  flow.flow_type = 0x02;
  flow.dst_port = 20000;

  /* -EPERM, not silence. The old manager dropped such a request and the client
   * waited for an answer that never came. */
  EXPECT_EQ(mtlm_lcore_get(client, 1), -EPERM);
  EXPECT_EQ(mtlm_lcore_put(client, 1), -EPERM);
  EXPECT_EQ(mtlm_queue_get(client, 1), -EPERM);
  EXPECT_EQ(mtlm_queue_put(client, 1, 1), -EPERM);
  EXPECT_EQ(mtlm_flow_add(client, &flow), -EPERM);
  EXPECT_EQ(mtlm_flow_del(client, 1, 1), -EPERM);
  EXPECT_EQ(mtlm_udp_dp_filter_add(client, 1, 20000), -EPERM);
  EXPECT_EQ(mtlm_udp_dp_filter_del(client, 1, 20000), -EPERM);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, HeartbeatNeedsNoRegistration) {
  manager_process manager(sock_path);

  mtlm_client* client = mtlm_client_create(sock_path.c_str());
  ASSERT_NE(client, nullptr) << strerror(errno);

  /* A client uses it to learn whether the manager still runs, which it must be
   * able to do before it registers. */
  for (uint32_t seq = 1; seq <= 3; seq++) {
    uint32_t acked = 0;
    EXPECT_EQ(mtlm_heartbeat(client, seq, &acked), 0);
    EXPECT_EQ(acked, seq);
  }

  mtlm_client_destroy(client);
}

/*
 * The lcores, which every backend needs.
 */

TEST_F(ManagerApiTest, LcoreGetAndPut) {
  manager_process manager(sock_path);

  mtlm_client* client = registered_client();
  ASSERT_NE(client, nullptr) << strerror(errno);

  EXPECT_EQ(mtlm_lcore_get(client, 3), 0);
  EXPECT_EQ(mtlm_lcore_put(client, 3), 0);
  /* A released lcore is free again, so a restarted instance gets it back. */
  EXPECT_EQ(mtlm_lcore_get(client, 3), 0);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, AnLcoreGoesToOneClientOnly) {
  manager_process manager(sock_path);

  mtlm_client* first = registered_client();
  mtlm_client* second = registered_client();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  ASSERT_EQ(mtlm_lcore_get(first, 5), 0);
  /* This is the whole point of the manager: two MTL instances on one host must
   * not pin a thread to the same core. */
  EXPECT_EQ(mtlm_lcore_get(second, 5), -EBUSY);
  /* And the other client cannot take it away either. */
  EXPECT_EQ(mtlm_lcore_put(second, 5), -EINVAL);
  EXPECT_EQ(mtlm_lcore_get(second, 6), 0);

  mtlm_client_destroy(first);
  mtlm_client_destroy(second);
}

TEST_F(ManagerApiTest, AClientThatLeavesGivesItsLcoresBack) {
  manager_process manager(sock_path);

  mtlm_client* first = registered_client();
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(mtlm_lcore_get(first, 7), 0);
  mtlm_client_destroy(first);

  mtlm_client* second = registered_client();
  ASSERT_NE(second, nullptr);
  /* An instance that crashes must not take a core out of the pool until the
   * host reboots. The manager notices the closed socket and frees it. */
  int ret = -EBUSY;
  for (int i = 0; i < 500 && ret == -EBUSY; i++) {
    ret = mtlm_lcore_get(second, 7);
    if (ret == -EBUSY) usleep(10 * 1000);
  }
  EXPECT_EQ(ret, 0);

  mtlm_client_destroy(second);
}

TEST_F(ManagerApiTest, AnLcorePutOfAnUnheldCoreIsRefused) {
  manager_process manager(sock_path);

  mtlm_client* client = registered_client();
  ASSERT_NE(client, nullptr);

  EXPECT_EQ(mtlm_lcore_put(client, 9), -EINVAL);

  mtlm_client_destroy(client);
}

/*
 * The device operations. Loopback has no combined channel and no rule table, so
 * these cases pin the error the manager reports.
 */

TEST_F(ManagerApiTest, QueueGetOnAnUnknownInterface) {
  manager_process manager(sock_path);

  mtlm_client* client = registered_client();
  ASSERT_NE(client, nullptr);

  EXPECT_EQ(mtlm_queue_get(client, 0xfffffff), -ENODEV);
  EXPECT_EQ(mtlm_queue_put(client, 0xfffffff, 1), -EINVAL);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, QueueGetOnAnInterfaceWithNoChannel) {
  unsigned int lo = if_nametoindex("lo");

  if (lo == 0) GTEST_SKIP() << "no loopback interface";

  manager_process manager(sock_path);
  mtlm_client* client = registered_client();
  ASSERT_NE(client, nullptr);

  /* Loopback answers no ethtool channel query, so the manager cannot take it
   * over. It must say so and stay up. */
  EXPECT_LT(mtlm_queue_get(client, lo), 0);
  EXPECT_EQ(mtlm_heartbeat(client, 1, nullptr), 0);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, FlowAddOnAnUnknownInterface) {
  manager_process manager(sock_path);
  struct mtlm_flow flow;

  mtlm_client* client = registered_client();
  ASSERT_NE(client, nullptr);

  memset(&flow, 0, sizeof(flow));
  flow.ifindex = 0xfffffff;
  flow.queue_id = 1;
  flow.flow_type = 0x02;
  flow.dst_ip = 0x0100005e; /* 239.0.0.1, network byte order */
  flow.dst_port = 20000;

  EXPECT_EQ(mtlm_flow_add(client, &flow), -ENODEV);
  EXPECT_EQ(mtlm_flow_del(client, flow.ifindex, 1), -EINVAL);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, FlowAddRefusesABadArgument) {
  manager_process manager(sock_path);

  mtlm_client* client = registered_client();
  ASSERT_NE(client, nullptr);

  EXPECT_EQ(mtlm_flow_add(client, nullptr), -EINVAL);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, UdpFilterOnAnUnknownInterface) {
  manager_process manager(sock_path);

  mtlm_client* client = registered_client();
  ASSERT_NE(client, nullptr);

  EXPECT_LT(mtlm_udp_dp_filter_add(client, 0xfffffff, 20000), 0);
  EXPECT_LT(mtlm_udp_dp_filter_del(client, 0xfffffff, 20000), 0);

  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, XskMapFdReportsWhyThereIsNone) {
  manager_process manager(sock_path);

  mtlm_client* client = mtlm_client_create(sock_path.c_str());
  ASSERT_NE(client, nullptr);

  /* A build with no libxdp, or an interface with no program, gives no
   * descriptor. The call must return an errno, not hang waiting for control
   * data that never arrives. */
  int fd = mtlm_xsk_map_fd(client, 0xfffffff);
  EXPECT_LT(fd, 0);
  if (fd >= 0) close(fd);

  EXPECT_EQ(mtlm_heartbeat(client, 1, nullptr), 0);
  mtlm_client_destroy(client);
}

/*
 * What the manager does with a client that misbehaves.
 */

TEST_F(ManagerApiTest, GarbageFromOneClientDoesNotStopTheManager) {
  manager_process manager(sock_path);

  mtlm_client* bad = mtlm_client_create(sock_path.c_str());
  ASSERT_NE(bad, nullptr);

  /* Not a record: a wrong magic, then a short write. The manager must close
   * this one connection only. */
  const char junk[64] = {0x41};
  ASSERT_GT(send(mtlm_client_fd(bad), junk, sizeof(junk), MSG_NOSIGNAL), 0);
  mtlm_client_destroy(bad);

  EXPECT_TRUE(manager.running());
  mtlm_client* good = registered_client();
  ASSERT_NE(good, nullptr) << strerror(errno);
  EXPECT_EQ(mtlm_lcore_get(good, 2), 0);
  mtlm_client_destroy(good);
}

TEST_F(ManagerApiTest, ManyClientsAtOnce) {
  manager_process manager(sock_path);
  std::vector<mtlm_client*> clients;

  for (int i = 0; i < 16; i++) {
    mtlm_client* client = registered_client();
    ASSERT_NE(client, nullptr) << "client " << i << ": " << strerror(errno);
    /* One lcore each, so the manager holds state for every one of them. */
    ASSERT_EQ(mtlm_lcore_get(client, static_cast<uint16_t>(i)), 0) << "client " << i;
    clients.push_back(client);
  }

  for (mtlm_client* client : clients) mtlm_client_destroy(client);
  EXPECT_TRUE(manager.running());
}

TEST_F(ManagerApiTest, ARestartedManagerServesTheSamePath) {
  {
    manager_process first(sock_path);
    mtlm_client* client = registered_client();
    ASSERT_NE(client, nullptr);
    ASSERT_EQ(mtlm_lcore_get(client, 4), 0);
    mtlm_client_destroy(client);
  }

  /* The lcore state lives in the process, so a restart frees everything. A
   * client of the old manager must not keep a core reserved. */
  manager_process second(sock_path);
  mtlm_client* client = registered_client();
  ASSERT_NE(client, nullptr) << strerror(errno);
  EXPECT_EQ(mtlm_lcore_get(client, 4), 0);
  mtlm_client_destroy(client);
}

TEST_F(ManagerApiTest, ASecondManagerOnTheSamePathStopsItself) {
  manager_process manager(sock_path);

  /* A second manager must not take the socket away from the first one. It
   * exits with a failure and leaves the live one serving. */
  std::string binary = manager_binary();
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    const char* args[] = {binary.c_str(), "--sock-path", sock_path.c_str(),
                          "--log-level",  "error",       nullptr};
    execvp(binary.c_str(), const_cast<char**>(args));
    _exit(127);
  }

  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_NE(WEXITSTATUS(status), 0);

  mtlm_client* client = registered_client();
  ASSERT_NE(client, nullptr) << strerror(errno);
  mtlm_client_destroy(client);
}
