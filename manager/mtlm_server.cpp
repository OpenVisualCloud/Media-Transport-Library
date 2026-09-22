/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "mtlm_server.hpp"

#include <grp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include "logging.hpp"
#include "mtlm_api.h"

namespace {

/* Size of one epoll_wait() batch. The loop runs again while events remain, so
 * this bounds the stack use, not the number of clients. */
constexpr int kMaxEvents = 64;

/* Read chunk. Several whole records fit, and mtl_instance::feed() keeps any
 * partial tail until the rest arrives. */
constexpr size_t kReadChunk = 4096;

int epoll_add(int epoll_fd, int fd) {
  struct epoll_event ev = {};

  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) return -errno;

  return 0;
}

} /* namespace */

mtlm_server::mtlm_server(const mtlm_server_config& config) : config(config) {
}

mtlm_server::~mtlm_server() {
  close_all();
}

void mtlm_server::close_all() {
  clients.clear(); /* each instance closes its own socket */
  registry.reset();

  if (signal_fd >= 0) {
    close(signal_fd);
    signal_fd = -1;
  }
  if (stop_fd >= 0) {
    close(stop_fd);
    stop_fd = -1;
  }
  if (epoll_fd >= 0) {
    close(epoll_fd);
    epoll_fd = -1;
  }
  if (listen_fd >= 0) {
    close(listen_fd);
    listen_fd = -1;
  }

  /* Take the socket file away only when this process is the one that made it.
   * Removing someone else's socket would cut off a running manager. */
  if (bound && !path.empty()) {
    unlink(path.c_str());
    bound = false;
  }
}

int mtlm_server::bind_socket() {
  struct sockaddr_un addr;

  if (path.size() >= sizeof(addr.sun_path)) {
    logger::log(log_level::ERROR,
                "The socket path is too long for an AF_UNIX address: " + path + " (" +
                    std::to_string(path.size()) + " of at most " +
                    std::to_string(sizeof(addr.sun_path) - 1) + ")");
    return -ENAMETOOLONG;
  }

  int ret = mtlm_sock_dir_prepare(path.c_str(), path_is_system ? 0755 : 0700);
  if (ret < 0) {
    logger::log(log_level::ERROR, "Failed to create the directory of " + path + ": " +
                                      strerror(-ret) +
                                      ". Choose a writable path with --sock-path or " +
                                      MTL_MANAGER_SOCK_ENV + ".");
    return ret;
  }

  /* A socket file left by a manager that crashed blocks the bind. Removing it
   * is safe only when nothing answers on it. */
  if (mtlm_manager_alive(path.c_str())) {
    logger::log(log_level::ERROR, "Another MTL Manager already answers on " + path + ".");
    return -EADDRINUSE;
  }
  unlink(path.c_str());

  listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    ret = -errno;
    logger::log(log_level::ERROR,
                "Failed to create the socket: " + std::string(strerror(-ret)));
    return ret;
  }

  /* Zero the whole address. A partly filled sun_path would make bind() use
   * bytes this function never set. */
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ret = -errno;
    logger::log(log_level::ERROR, "Failed to bind " + path + ": " + strerror(-ret) +
                                      ". Run with the rights for that path, or set " +
                                      MTL_MANAGER_SOCK_ENV + " to one you own.");
    return ret;
  }
  bound = true;

  ret = apply_socket_permissions();
  if (ret < 0) return ret;

  if (listen(listen_fd, static_cast<int>(config.max_clients)) < 0) {
    ret = -errno;
    logger::log(log_level::ERROR,
                "Failed to listen on the socket: " + std::string(strerror(-ret)));
    return ret;
  }

  return 0;
}

int mtlm_server::apply_socket_permissions() {
  /* A client needs write permission on the socket file to connect. The mask
   * keeps a caller that gave a larger number from making the log text disagree
   * with what chmod does. */
  mode_t mode = config.socket_mode != 0 ? static_cast<mode_t>(config.socket_mode & 07777)
                                        : (path_is_system ? 0666 : 0600);

  if (!config.socket_group.empty()) {
    struct group* grp = getgrnam(config.socket_group.c_str());
    if (grp == nullptr) {
      logger::log(log_level::ERROR, "No group named " + config.socket_group + ".");
      return -ENOENT;
    }
    if (chown(path.c_str(), static_cast<uid_t>(-1), grp->gr_gid) < 0) {
      int ret = -errno;
      logger::log(log_level::ERROR, "Failed to give the socket to group " +
                                        config.socket_group + ": " + strerror(-ret));
      return ret;
    }
    /* A named group is a request to keep everyone else out. */
    if (config.socket_mode == 0) mode = 0660;
  }

  if (chmod(path.c_str(), mode) < 0) {
    int ret = -errno;
    logger::log(log_level::ERROR,
                "Failed to set the mode of the socket: " + std::string(strerror(-ret)));
    return ret;
  }

  char mode_text[16];
  snprintf(mode_text, sizeof(mode_text), "0%o", mode);
  logger::log(log_level::INFO,
              "Socket " + path + " mode " + mode_text +
                  (config.socket_group.empty() ? "" : ", group " + config.socket_group));
  return 0;
}

int mtlm_server::arm_signals() {
  sigset_t mask;

  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM); /* systemd stops a service with SIGTERM */

  if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
    int ret = -errno;
    logger::log(log_level::ERROR,
                "Failed to block the signals: " + std::string(strerror(-ret)));
    return ret;
  }

  /* A client that disconnects mid write must not end the process. Every send
   * also passes MSG_NOSIGNAL, so this is the second line of defence. */
  signal(SIGPIPE, SIG_IGN);

  signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
  if (signal_fd < 0) {
    int ret = -errno;
    logger::log(log_level::ERROR,
                "Failed to create the signal descriptor: " + std::string(strerror(-ret)));
    return ret;
  }

  return 0;
}

int mtlm_server::setup() {
  if (config.sock_path.empty()) {
    const char* resolved = mtlm_sock_path();
    if (resolved == nullptr) {
      logger::log(log_level::ERROR, "Cannot work out a socket path.");
      return -ENAMETOOLONG;
    }
    path = resolved;
  } else {
    path = config.sock_path;
  }
  path_is_system = mtlm_sock_path_is_system(path.c_str());

  int ret = arm_signals();
  if (ret < 0) return ret;

  ret = bind_socket();
  if (ret < 0) return ret;

  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    ret = -errno;
    logger::log(log_level::ERROR,
                "Failed to create the epoll descriptor: " + std::string(strerror(-ret)));
    return ret;
  }

  ret = epoll_add(epoll_fd, signal_fd);
  if (ret < 0) {
    logger::log(log_level::ERROR, "Failed to watch the signal descriptor.");
    return ret;
  }

  ret = epoll_add(epoll_fd, listen_fd);
  if (ret < 0) {
    logger::log(log_level::ERROR, "Failed to watch the listening socket.");
    return ret;
  }

  /* stop() writes here. Without it a stop request would sit unseen until the
   * next client event, because epoll_wait has no timeout. */
  stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (stop_fd < 0) {
    ret = -errno;
    logger::log(log_level::ERROR,
                "Failed to create the stop descriptor: " + std::string(strerror(-ret)));
    return ret;
  }

  ret = epoll_add(epoll_fd, stop_fd);
  if (ret < 0) {
    logger::log(log_level::ERROR, "Failed to watch the stop descriptor.");
    return ret;
  }

  netdev = mtlm_netdev_linux();
  registry.reset(new mtl_interface_registry(netdev));

  return 0;
}

int mtlm_server::accept_client() {
  int fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (fd < 0) return -errno;

  /* Bound the number of connections. listen() only bounds the backlog, so
   * without this one client could open sockets until the process runs out. */
  if (clients.size() >= config.max_clients) {
    logger::log(log_level::WARNING, "Refused a client: already at the limit of " +
                                        std::to_string(config.max_clients) + ".");
    close(fd);
    return -EMFILE;
  }

  int ret = epoll_add(epoll_fd, fd);
  if (ret < 0) {
    logger::log(log_level::ERROR, "Failed to watch a client socket.");
    close(fd);
    return ret;
  }

  clients.push_back(std::unique_ptr<mtl_instance>(
      new mtl_instance(fd, *registry, mtl_lcore::get_instance())));
  logger::log(log_level::INFO, "New client connected, fd " + std::to_string(fd) +
                                   ", total " + std::to_string(clients.size()));
  return 0;
}

void mtlm_server::drop_client(int fd) {
  auto it = std::find_if(clients.begin(), clients.end(),
                         [fd](const std::unique_ptr<mtl_instance>& client) {
                           return client->get_conn_fd() == fd;
                         });
  if (it == clients.end()) return;

  /* Stop watching before the destructor closes the descriptor. */
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
  clients.erase(it);
  logger::log(log_level::INFO, "Total clients: " + std::to_string(clients.size()));
}

bool mtlm_server::service_client(int fd) {
  auto it = std::find_if(clients.begin(), clients.end(),
                         [fd](const std::unique_ptr<mtl_instance>& client) {
                           return client->get_conn_fd() == fd;
                         });
  if (it == clients.end()) return false;

  char buf[kReadChunk];
  ssize_t len = recv(fd, buf, sizeof(buf), 0);
  if (len < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return true;
    logger::log(log_level::ERROR, "Failed to read from client " + std::to_string(fd) +
                                      ": " + strerror(errno));
    return false;
  }
  if (len == 0) {
    logger::log(log_level::INFO, "Client " + std::to_string(fd) + " disconnected.");
    return false;
  }

  return (*it)->feed(buf, static_cast<size_t>(len)) >= 0;
}

int mtlm_server::run() {
  struct epoll_event events[kMaxEvents];

  if (epoll_fd < 0 || listen_fd < 0) return -EINVAL;

  running = true;
  logger::log(log_level::INFO, "MTL Manager is running on " + path +
                                   ". Send SIGINT or SIGTERM to stop it.");
  if (!mtlm_xdp_supported())
    logger::log(log_level::WARNING, "No XDP support in this build.");

  while (running) {
    int nfds = epoll_wait(epoll_fd, events, kMaxEvents, -1);
    if (nfds < 0) {
      if (errno == EINTR) continue;
      int ret = -errno;
      logger::log(log_level::ERROR, "epoll_wait failed: " + std::string(strerror(-ret)));
      return ret;
    }

    for (int i = 0; i < nfds && running; i++) {
      int fd = events[i].data.fd;

      if (fd == listen_fd) {
        accept_client();
      } else if (fd == stop_fd) {
        uint64_t count = 0;
        ssize_t got = read(stop_fd, &count, sizeof(count));
        (void)got; /* the value carries nothing: any write means stop */
        logger::log(log_level::INFO, "Asked to stop. Shutting down.");
        running = false;
      } else if (fd == signal_fd) {
        struct signalfd_siginfo siginfo;
        ssize_t len = read(signal_fd, &siginfo, sizeof(siginfo));
        if (len != static_cast<ssize_t>(sizeof(siginfo))) {
          /* Do not return here: the destructor must still take the socket
           * file away. Treat it as a stop request. */
          logger::log(log_level::ERROR, "Failed to read the signal.");
          running = false;
          break;
        }
        logger::log(
            log_level::INFO,
            "Received signal " + std::to_string(siginfo.ssi_signo) + ". Shutting down.");
        running = false;
      } else if (!service_client(fd)) {
        drop_client(fd);
      }
    }
  }

  logger::log(log_level::INFO, "MTL Manager exited.");
  return 0;
}

void mtlm_server::stop() {
  running = false;

  if (stop_fd < 0) return;

  uint64_t one = 1;
  ssize_t ret = write(stop_fd, &one, sizeof(one));
  (void)ret; /* a full counter already means a pending stop */
}
