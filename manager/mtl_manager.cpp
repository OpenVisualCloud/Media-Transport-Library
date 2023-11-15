/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/un.h>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
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
    std::cerr << "Failed to block signals." << std::endl;
    return ret;
  }

  int signal_fd = signalfd(-1, &signal_mask, 0);
  if (signal_fd == -1) {
    std::cerr << "Failed to create signalfd." << std::endl;
    ret = signal_fd;
    goto out;
  }

  sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockfd < 0) {
    std::cerr << "Failed to create socket." << std::endl;
    ret = sockfd;
    goto out;
  }

  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, MTL_MANAGER_SOCK_PATH, sizeof(addr.sun_path) - 1);

  unlink(MTL_MANAGER_SOCK_PATH);

  ret = bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
  if (ret < 0) {
    std::cerr << "Failed to bind socket." << std::endl;
    goto out;
  }

  /* Allow all users to connect (which might be insecure) */
  fs::permissions(MTL_MANAGER_SOCK_PATH, fs::perms::all, fs::perm_options::replace);

  ret = listen(sockfd, 10);
  if (ret < 0) {
    std::cerr << "Failed to listen socket." << std::endl;
    goto out;
  }

  epfd = epoll_create1(0);
  if (epfd < 0) {
    std::cerr << "Failed to create epoll fd." << std::endl;
    ret = epfd;
    goto out;
  }

  struct epoll_event event;
  event.data.fd = signal_fd;
  event.events = EPOLLIN;

  ret = epoll_ctl(epfd, EPOLL_CTL_ADD, signal_fd, &event);
  if (ret == -1) {
    std::cerr << "Failed to add signal_fd to epoll." << std::endl;
    goto out;
  }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = sockfd;
  ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);
  if (ret == -1) {
    std::cerr << "Failed to add server socket to epoll." << std::endl;
    goto out;
  }

  is_running.store(true);
  std::cout << "MTL Manager is running. Press Ctrl+C to stop it." << std::endl;

  while (is_running.load()) {
    std::cout << "Connected client numbers: " << clients.size() << std::endl;
    struct epoll_event events[clients.size() + 2];
    int nfds = epoll_wait(epfd, events, clients.size() + 2, -1);
    if (nfds < 0) {
      std::cerr << "Failed to wait epoll." << std::endl;
      continue;
    }
    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == sockfd) {
        int client_sockfd = accept(sockfd, NULL, NULL);
        if (client_sockfd < 0) {
          std::cerr << "Failed to accept client." << std::endl;
          continue;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = client_sockfd;
        ret = epoll_ctl(epfd, EPOLL_CTL_ADD, client_sockfd, &ev);
        if (ret < 0) {
          std::cerr << "Failed to add client to epoll." << std::endl;
          close(client_sockfd);
          continue;
        }

        mtl_instance* client = new mtl_instance(client_sockfd, lcore_manager);
        clients.push_back(client);
        std::cout << "New client: " << client_sockfd << std::endl;
      } else if (events[i].data.fd == signal_fd) {
        struct signalfd_siginfo siginfo;
        ssize_t len = read(signal_fd, &siginfo, sizeof(siginfo));
        if (len != sizeof(siginfo)) {
          std::cerr << "Failed to read signal info." << std::endl;
          return 1;
        }

        if (siginfo.ssi_signo == SIGINT) {
          std::cout << "Ctrl+C detected. Exiting..." << std::endl;
          is_running.store(false);
        }
      } else {
        for (auto it = clients.begin(); it != clients.end();) {
          mtl_instance* client = *it;
          if (client->get_conn_fd() == events[i].data.fd) {
            char buf[256];
            int len = recv(events[i].data.fd, buf, sizeof(buf), 0);
            if (len < 0) {
              std::cerr << "Failed to receive message from client." << std::endl;
              ++it;
              continue;
            }
            if (len == 0) {
              std::cout << "Client " << events[i].data.fd << " disconnected."
                        << std::endl;
              ret = epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
              if (ret < 0)
                std::cerr << "Failed to remove client from epoll." << std::endl;
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

  std::cout << "MTL Manager exited." << std::endl;

out:
  clients.clear();
  if (signal_fd >= 0) close(signal_fd);
  if (epfd >= 0) close(epfd);
  if (sockfd >= 0) close(sockfd);
  return ret;
}