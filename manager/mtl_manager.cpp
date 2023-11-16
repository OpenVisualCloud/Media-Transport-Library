/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/un.h>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <memory>
#include <vector>

#include "mtl_instance.hpp"
#include "mtl_mproto.h"

namespace fs = std::filesystem;

static std::atomic<bool> is_running(true);

int main() {
  int ret = 0;
  int epfd = -1, sockfd = -1;
  std::vector<mtl_instance*> clients;
  std::shared_ptr<mtl_lcore> lcore_manager = std::make_shared<mtl_lcore>();

  logger::set_log_level(log_level::INFO);

  fs::path directory_path(MTL_MANAGER_SOCK_PATH);
  directory_path.remove_filename();
  if (!fs::exists(directory_path)) {
    fs::create_directory(directory_path);
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

  ret = bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
  if (ret < 0) {
    logger::log(log_level::ERROR, "Failed to bind socket.");
    goto out;
  }

  /* Allow all users to connect (which might be insecure) */
  fs::permissions(MTL_MANAGER_SOCK_PATH, fs::perms::all, fs::perm_options::replace);

  ret = listen(sockfd, 10);
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

  is_running.store(true);
  logger::log(log_level::INFO, "MTL Manager is running. Press Ctrl+C to stop it.");

  while (is_running.load()) {
    logger::log(log_level::INFO,
                "Listening " + std::to_string(clients.size()) + " clients");
    struct epoll_event events[clients.size() + 2];
    int nfds = epoll_wait(epfd, events, clients.size() + 2, -1);
    if (nfds < 0) {
      logger::log(log_level::ERROR, "Failed to wait for epoll.");
      continue;
    }
    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == sockfd) {
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

        mtl_instance* client = new mtl_instance(client_sockfd, lcore_manager);
        clients.push_back(client);
        logger::log(log_level::INFO,
                    "New client connected. fd: " + std::to_string(client_sockfd));
      } else if (events[i].data.fd == signal_fd) {
        struct signalfd_siginfo siginfo;
        ssize_t len = read(signal_fd, &siginfo, sizeof(siginfo));
        if (len != sizeof(siginfo)) {
          logger::log(log_level::ERROR, "Failed to read signal.");
          return 1;
        }

        if (siginfo.ssi_signo == SIGINT) {
          logger::log(log_level::INFO, "Received SIGINT. Shutting down.");
          is_running.store(false);
        }
      } else {
        for (auto it = clients.begin(); it != clients.end();) {
          mtl_instance* client = *it;
          if (client->get_conn_fd() == events[i].data.fd) {
            char buf[256];
            int len = recv(events[i].data.fd, buf, sizeof(buf), 0);
            if (len < 0) {
              logger::log(log_level::ERROR, "Failed to receive data from client " +
                                                std::to_string(events[i].data.fd));
              ++it;
              continue;
            }
            if (len == 0) {
              logger::log(log_level::INFO, "Client " + std::to_string(events[i].data.fd) +
                                               " disconnected.");
              ret = epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
              if (ret < 0)
                logger::log(log_level::ERROR, "Failed to remove client from epoll.");
              delete client; /* close fd in deconstruction */
              it = clients.erase(it);
              continue;
            }
            client->handle_message(buf, len);
          }
          ++it;
        }
      }
    }
  }

out:
  for (auto it = clients.begin(); it != clients.end(); ++it) {
    delete *it;
  }
  if (signal_fd >= 0) close(signal_fd);
  if (epfd >= 0) close(epfd);
  if (sockfd >= 0) close(sockfd);

  logger::log(log_level::INFO, "MTL Manager exited.");
  return ret;
}