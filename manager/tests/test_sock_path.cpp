/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/* The socket path rules, which is what lets the manager run without root. */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>

#include "mtlm_api.h"

namespace {

/* Set or clear one environment variable, and put it back afterwards. */
class env_guard {
 public:
  env_guard(const char* name, const char* value) : name(name) {
    const char* old = getenv(name);

    had_old = old != nullptr;
    if (had_old) old_value = old;

    if (value == nullptr)
      unsetenv(name);
    else
      setenv(name, value, 1);
  }

  ~env_guard() {
    if (had_old)
      setenv(name.c_str(), old_value.c_str(), 1);
    else
      unsetenv(name.c_str());
  }

 private:
  std::string name;
  std::string old_value;
  bool had_old;
};

std::string candidate(unsigned int index) {
  char buf[MTLM_SOCK_PATH_MAX];

  if (mtlm_sock_path_candidate(index, buf, sizeof(buf)) < 0) return std::string();
  return std::string(buf);
}

} /* namespace */

TEST(SockPath, EnvOverrideIsTheOnlyCandidate) {
  env_guard sock(MTL_MANAGER_SOCK_ENV, "/tmp/mtlm-test-env.sock");

  EXPECT_EQ(candidate(0), "/tmp/mtlm-test-env.sock");
  /* A second candidate would send a client to a manager nobody asked for. */
  char buf[MTLM_SOCK_PATH_MAX];
  EXPECT_EQ(mtlm_sock_path_candidate(1, buf, sizeof(buf)), -ENOENT);
}

TEST(SockPath, EmptyEnvIsIgnored) {
  env_guard sock(MTL_MANAGER_SOCK_ENV, "");

  EXPECT_NE(candidate(0), "");
  EXPECT_NE(candidate(0), " ");
}

TEST(SockPath, NonRootPrefersThePerUserPath) {
  if (geteuid() == 0) GTEST_SKIP() << "This case describes a user who is not root.";

  env_guard sock(MTL_MANAGER_SOCK_ENV, nullptr);
  env_guard runtime("XDG_RUNTIME_DIR", "/run/user/12345");

  EXPECT_EQ(candidate(0), std::string("/run/user/12345/imtl/") + MTL_MANAGER_SOCK_NAME);
  /* The system manager stays reachable, second in the order. */
  EXPECT_EQ(candidate(1), MTL_MANAGER_SOCK_PATH);
}

TEST(SockPath, NoRuntimeDirFallsBackToTmp) {
  if (geteuid() == 0) GTEST_SKIP() << "This case describes a user who is not root.";

  env_guard sock(MTL_MANAGER_SOCK_ENV, nullptr);
  env_guard runtime("XDG_RUNTIME_DIR", nullptr);

  std::string expect =
      "/tmp/imtl-" + std::to_string(getuid()) + "/" + MTL_MANAGER_SOCK_NAME;
  EXPECT_EQ(candidate(0), expect);
}

TEST(SockPath, RelativeRuntimeDirIsRefused) {
  if (geteuid() == 0) GTEST_SKIP() << "This case describes a user who is not root.";

  env_guard sock(MTL_MANAGER_SOCK_ENV, nullptr);
  /* A relative XDG_RUNTIME_DIR would make a socket in the current directory. */
  env_guard runtime("XDG_RUNTIME_DIR", "not-absolute");

  std::string expect =
      "/tmp/imtl-" + std::to_string(getuid()) + "/" + MTL_MANAGER_SOCK_NAME;
  EXPECT_EQ(candidate(0), expect);
}

TEST(SockPath, ResolveRejectsABadArgument) {
  char buf[MTLM_SOCK_PATH_MAX];

  EXPECT_EQ(mtlm_sock_path_resolve(nullptr, sizeof(buf)), -EINVAL);
  EXPECT_EQ(mtlm_sock_path_resolve(buf, 0), -EINVAL);
}

TEST(SockPath, ResolveRefusesASmallBuffer) {
  env_guard sock(MTL_MANAGER_SOCK_ENV, "/tmp/mtlm-test-a-long-enough-name.sock");
  char buf[8];

  /* A silent truncation would name a different socket. */
  EXPECT_EQ(mtlm_sock_path_resolve(buf, sizeof(buf)), -ENAMETOOLONG);
}

TEST(SockPath, ResolveRefusesAPathAnAddressCannotHold) {
  std::string too_long = "/tmp/" + std::string(MTLM_SOCK_PATH_MAX, 'x');
  env_guard sock(MTL_MANAGER_SOCK_ENV, too_long.c_str());
  char buf[MTLM_SOCK_PATH_MAX * 2];

  EXPECT_EQ(mtlm_sock_path_resolve(buf, sizeof(buf)), -ENAMETOOLONG);
}

TEST(SockPath, StaticResolveMatchesTheFirstCandidate) {
  env_guard sock(MTL_MANAGER_SOCK_ENV, "/tmp/mtlm-test-static.sock");

  ASSERT_NE(mtlm_sock_path(), nullptr);
  EXPECT_STREQ(mtlm_sock_path(), "/tmp/mtlm-test-static.sock");
}

TEST(SockPath, IsSystemNamesOnlyTheSystemPath) {
  EXPECT_TRUE(mtlm_sock_path_is_system(MTL_MANAGER_SOCK_PATH));
  EXPECT_FALSE(mtlm_sock_path_is_system("/tmp/imtl-1000/mtl_manager.sock"));
  EXPECT_FALSE(mtlm_sock_path_is_system(nullptr));
}

TEST(SockPath, DirPrepareMakesEveryMissingComponent) {
  std::string dir = "/tmp/mtlm-test-" + std::to_string(getpid());
  std::string path = dir + "/a/b/mtl_manager.sock";
  /* The mode this case checks is the mode asked for, not the mode the umask of
   * the moment leaves behind. */
  mode_t old_umask = umask(0);

  ASSERT_EQ(mtlm_sock_dir_prepare(path.c_str(), 0700), 0);
  umask(old_umask);

  struct stat st = {};
  ASSERT_EQ(stat((dir + "/a/b").c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(st.st_mode & 0777, 0700u);

  /* Running it again on a directory that exists must still report success. */
  EXPECT_EQ(mtlm_sock_dir_prepare(path.c_str(), 0700), 0);

  rmdir((dir + "/a/b").c_str());
  rmdir((dir + "/a").c_str());
  rmdir(dir.c_str());
}

TEST(SockPath, DirPrepareRejectsAName) {
  EXPECT_EQ(mtlm_sock_dir_prepare(nullptr, 0700), -EINVAL);
  /* No slash means no directory to make. */
  EXPECT_EQ(mtlm_sock_dir_prepare("mtl_manager.sock", 0700), -EINVAL);
  /* The root directory is already there. */
  EXPECT_EQ(mtlm_sock_dir_prepare("/mtl_manager.sock", 0700), 0);
}

TEST(SockPath, ProtoVersionReportsMajorAndMinor) {
  std::string expect = std::to_string(MTL_MANAGER_PROTO_VERSION_MAJOR) + "." +
                       std::to_string(MTL_MANAGER_PROTO_VERSION_MINOR);

  EXPECT_EQ(std::string(mtlm_proto_version()), expect);
}
