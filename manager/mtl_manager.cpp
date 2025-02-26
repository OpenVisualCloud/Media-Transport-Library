/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/un.h>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <memory>
#include <vector>

#include "logging.hpp"
#include "mtl_instance.hpp"
#include "mtl_mproto.h"
#include "mtlm_build_config.h"

namespace fs = std::filesystem;

static const int MAX_CLIENTS = 10;

static const char *mtlm_version(void) {
  static char version[128];
  if (version[0] != 0) return version;

  snprintf(version, sizeof(version), "%d.%d.%d.%s %s %s %s", MTLM_VERSION_MAJOR,
           MTLM_VERSION_MINOR, MTLM_VERSION_LAST, MTLM_VERSION_EXTRA, __TIMESTAMP__,
           __MTLM_GIT__, MTLM_COMPILER);

  return version;
}

int main() {
  int ret = 0;
  bool is_running = true;
  int epfd = -1, sockfd = -1;
  std::vector<std::unique_ptr<mtl_instance>> clients;

  logger::set_log_level(log_level::INFO);
  logger::log(log_level::INFO, "MTL Manager version: " + std::string(mtlm_version()));

  fs::path directory_path(MTL_MANAGER_SOCK_PATH);
  directory_path.remove_filename();
  if (!fs::exists(directory_path)) {
    try {
      fs::create_directory(directory_path);
    } catch (const std::exception &e) {
      logger::log(log_level::ERROR,
                  "Failed to create dir:" + std::string(MTL_MANAGER_SOCK_PATH) +
                      ", please run the application with the appropriate privileges");
      return -EIO;
    }
  }

  sigset_t signal_mask;
  sigemptyset(&signal_mask);
  sigaddset(&signal_mask, SIGINT);

  ret = sigprocmask(SIG_BLOCK, &signal_mask, NULL);
  if (ret == -1) {
    logger::log(log_level::ERROR, "Failed to set signal mask.");
    return ret;
  }

  int signal_fd = signalfd(-1, &signal_mask, 0);
  if (signal_fd == -1) {
    logger::log(log_level::ERROR, "Failed to create signal fd.");
    ret = signal_fd;
    goto out;
  }

  sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockfd < 0) {
    logger::log(log_level::ERROR, "Failed to create socket.");
    ret = sockfd;
    goto out;
  }

  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, MTL_MANAGER_SOCK_PATH, sizeof(addr.sun_path) - 1);

  unlink(MTL_MANAGER_SOCK_PATH);

  ret = bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0) {
    logger::log(log_level::ERROR,
                "Failed to bind socket, please run the application with the "
                "appropriate privileges.");
    goto out;
  }

  /* Allow all users to connect (which might be insecure) */
  fs::permissions(MTL_MANAGER_SOCK_PATH, fs::perms::all, fs::perm_options::replace);

  ret = listen(sockfd, MAX_CLIENTS);
  if (ret < 0) {
    logger::log(log_level::ERROR, "Failed to listen on socket.");
    goto out;
  }

  epfd = epoll_create1(0);
  if (epfd < 0) {
    logger::log(log_level::ERROR, "Failed to create epoll.");
    ret = epfd;
    goto out;
  }

  struct epoll_event event;
  event.data.fd = signal_fd;
  event.events = EPOLLIN;

  ret = epoll_ctl(epfd, EPOLL_CTL_ADD, signal_fd, &event);
  if (ret == -1) {
    logger::log(log_level::ERROR, "Failed to add signal fd to epoll.");
    goto out;
  }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = sockfd;
  ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);
  if (ret == -1) {
    logger::log(log_level::ERROR, "Failed to add socket fd to epoll.");
    goto out;
  }

  logger::log(log_level::INFO,
              "MTL Manager is running. Press Ctrl+C or use SIGINT to stop it.");
#ifndef MTL_HAS_XDP_BACKEND
  logger::log(log_level::WARNING, "No XDP support for this build");
#endif

  while (is_running) {
    std::vector<struct epoll_event> events(clients.size() + 2);

    int nfds = epoll_wait(epfd, events.data(), clients.size() + 2, -1);
    if (nfds < 0) {
      logger::log(log_level::ERROR, "Failed to wait for epoll.");
      continue;
    }

    for (int i = 0; i < nfds; i++) {
      int evfd = events[i].data.fd;
      if (evfd == sockfd) { /* accept new client */
        int client_sockfd = accept(sockfd, NULL, NULL);
        if (client_sockfd < 0) {
          logger::log(log_level::ERROR, "Failed to accept client.");
          continue;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = client_sockfd;
        ret = epoll_ctl(epfd, EPOLL_CTL_ADD, client_sockfd, &ev);
        if (ret < 0) {
          logger::log(log_level::ERROR, "Failed to add client socket fd to epoll.");
          close(client_sockfd);
          continue;
        }

        auto client = std::make_unique<mtl_instance>(client_sockfd);
        clients.push_back(std::move(client));
        logger::log(log_level::INFO,
                    "New client connected. fd: " + std::to_string(client_sockfd));
        logger::log(log_level::INFO, "Total clients: " + std::to_string(clients.size()));
      } else if (evfd == signal_fd) { /* handle signal */
        struct signalfd_siginfo siginfo;
        ssize_t len = read(signal_fd, &siginfo, sizeof(siginfo));
        if (len != sizeof(siginfo)) {
          logger::log(log_level::ERROR, "Failed to read signal.");
          return 1;
        }

        if (siginfo.ssi_signo == SIGINT) {
          logger::log(log_level::INFO, "Received SIGINT. Shutting down.");
          is_running = false;
        }
      } else { /* handle client message */
        auto it = std::find_if(clients.begin(), clients.end(), [&](auto &client) {
          return client->get_conn_fd() == evfd;
        });
        if (it != clients.end()) {
          auto &client = *it;
          char buf[256];
          int len = recv(evfd, buf, sizeof(buf), 0);
          if (len < 0) {
            logger::log(log_level::ERROR,
                        "Failed to receive data from client " + std::to_string(evfd));
          } else if (len == 0) {
            logger::log(log_level::INFO,
                        "Client " + std::to_string(evfd) + " disconnected.");
            ret = epoll_ctl(epfd, EPOLL_CTL_DEL, evfd, NULL);
            if (ret < 0)
              logger::log(log_level::WARNING, "Failed to remove client from epoll.");
            clients.erase(it);
            logger::log(log_level::INFO,
                        "Total clients: " + std::to_string(clients.size()));
          } else {
            client->handle_message(buf, len);
          }
        }
      }
    }
  }
  logger::log(log_level::INFO, "MTL Manager exited.");

out:
  if (signal_fd >= 0) close(signal_fd);
  if (epfd >= 0) close(epfd);
  if (sockfd >= 0) close(sockfd);

  return ret;
}