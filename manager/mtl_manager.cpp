/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/*
 * MtlManager: the command line around mtlm_server.
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "logging.hpp"
#include "mtlm_api.h"
#include "mtlm_build_config.h"
#include "mtlm_server.hpp"

static const char* mtlm_version(void) {
  static char version[128];
  if (version[0] != 0) return version;

  snprintf(version, sizeof(version), "%d.%d.%d.%s %s %s %s", MTLM_VERSION_MAJOR,
           MTLM_VERSION_MINOR, MTLM_VERSION_LAST, MTLM_VERSION_EXTRA, __TIMESTAMP__,
           __MTLM_GIT__, MTLM_COMPILER);

  return version;
}

static void usage(const char* prog) {
  printf(
      "Usage: %s [options]\n"
      "\n"
      "Arbitrates the lcores, the receive queues, the ethtool flow rules and the\n"
      "XDP programs that the MTL instances on this host share.\n"
      "\n"
      "Options:\n"
      "  --sock-path PATH      Bind PATH instead of the resolved default.\n"
      "  --socket-mode OCTAL   Mode of the socket file, for example 660.\n"
      "  --socket-group NAME   Give the socket to group NAME, and use mode 0660\n"
      "                        unless --socket-mode says otherwise.\n"
      "  --max-clients N       Largest number of instances at once. Default 64.\n"
      "  --log-level LEVEL     debug, info, warning or error. Default info.\n"
      "  --print-sock-path     Print the path that would be bound, then exit.\n"
      "  --version             Print the version, then exit.\n"
      "  --help                Print this text, then exit.\n"
      "\n"
      "Root is not needed. Without it the socket goes below XDG_RUNTIME_DIR, and\n"
      "the operations that do need privilege, which are the XDP programs and the\n"
      "ethtool flow rules, report the refusal per request. The path in order:\n"
      "  1. $%s\n"
      "  2. %s, for root\n"
      "  3. $XDG_RUNTIME_DIR/imtl/%s\n"
      "  4. /tmp/imtl-<uid>/%s\n",
      prog, MTL_MANAGER_SOCK_ENV, MTL_MANAGER_SOCK_PATH, MTL_MANAGER_SOCK_NAME,
      MTL_MANAGER_SOCK_NAME);
}

/* Read the value that follows an option. Returns nullptr and complains when
 * the value is missing. */
static const char* next_arg(int argc, char** argv, int& i) {
  if (i + 1 >= argc) {
    fprintf(stderr, "%s needs a value\n", argv[i]);
    return nullptr;
  }
  return argv[++i];
}

int main(int argc, char** argv) {
  mtlm_server_config config;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    } else if (arg == "--version" || arg == "-v") {
      printf("MTL Manager version: %s, protocol %s\n", mtlm_version(),
             mtlm_proto_version());
      return 0;
    } else if (arg == "--print-sock-path") {
      const char* path = mtlm_sock_path();
      if (path == nullptr) {
        fprintf(stderr, "Cannot work out a socket path\n");
        return 1;
      }
      printf("%s\n", path);
      return 0;
    } else if (arg == "--sock-path") {
      const char* value = next_arg(argc, argv, i);
      if (value == nullptr) return 1;
      config.sock_path = value;
    } else if (arg == "--socket-mode") {
      const char* value = next_arg(argc, argv, i);
      if (value == nullptr) return 1;
      char* end = nullptr;
      unsigned long mode = strtoul(value, &end, 8);
      if (end == value || *end != '\0' || mode > 07777) {
        fprintf(stderr, "--socket-mode wants an octal mode, for example 660\n");
        return 1;
      }
      config.socket_mode = static_cast<unsigned int>(mode);
    } else if (arg == "--socket-group") {
      const char* value = next_arg(argc, argv, i);
      if (value == nullptr) return 1;
      config.socket_group = value;
    } else if (arg == "--max-clients") {
      const char* value = next_arg(argc, argv, i);
      if (value == nullptr) return 1;
      char* end = nullptr;
      unsigned long count = strtoul(value, &end, 10);
      if (end == value || *end != '\0' || count == 0 || count > 4096) {
        fprintf(stderr, "--max-clients wants a number from 1 to 4096\n");
        return 1;
      }
      config.max_clients = static_cast<size_t>(count);
    } else if (arg == "--log-level") {
      const char* value = next_arg(argc, argv, i);
      if (value == nullptr) return 1;
      if (!logger::set_log_level(std::string(value))) {
        fprintf(stderr, "--log-level wants debug, info, warning or error\n");
        return 1;
      }
    } else {
      fprintf(stderr, "Unknown option %s\n", argv[i]);
      usage(argv[0]);
      return 1;
    }
  }

  logger::log(log_level::INFO, "MTL Manager version: " + std::string(mtlm_version()) +
                                   ", protocol " + mtlm_proto_version());

  mtlm_server server(config);

  int ret = server.setup();
  if (ret < 0) return -ret > 255 ? 1 : -ret;

  ret = server.run();
  return ret < 0 ? 1 : 0;
}
