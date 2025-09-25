/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include "log.h"
#include "upl_test.h"

struct loop_para {
  int sessions;
  uint16_t udp_port;
  int udp_len;
  int tx_pkts;
  int max_rx_timeout_pkts;
  int tx_sleep_us;
  int rx_timeout_us;

  bool dual_loop;
  bool mcast;
  bool use_poll;
  bool use_select;
  bool use_epoll;
  bool mix_fd;
  bool sendmsg;
  bool recvmsg;
  bool sendmsg_gso;
  bool reuse_port;
  int reuse_tx_sessions;
};

static int loop_para_init(struct loop_para* para) {
  memset(para, 0x0, sizeof(*para));
  para->sessions = 1;
  para->udp_port = 10000;
  para->udp_len = 1024;
  para->tx_pkts = 1024;
  para->max_rx_timeout_pkts = para->tx_pkts / 100;
  para->tx_sleep_us = 1000;
  para->rx_timeout_us = 1000;
  para->dual_loop = false;
  para->mcast = false;
  para->use_poll = false;
  para->use_select = false;
  para->use_epoll = false;
  para->mix_fd = false;
  para->sendmsg = false;
  para->recvmsg = false;
  para->reuse_port = false;
  return 0;
}

static int loop_sanity_test(struct uplt_ctx* ctx, struct loop_para* para) {
  int tx_sessions = para->sessions;
  if (para->reuse_port) tx_sessions = para->reuse_tx_sessions;
  int rx_sessions = para->sessions;
  uint16_t udp_port = para->udp_port;
  int udp_len = para->udp_len;
  bool dual_loop = para->dual_loop;

  std::vector<int> tx_fds(tx_sessions);
  std::vector<int> rx_fds(rx_sessions);
  std::vector<int> rx_timeout(rx_sessions);
  std::vector<int> rx_pkts(rx_sessions);
  int total_rx_pkts = 0;
  std::vector<struct sockaddr_in> tx_addr(tx_sessions);
  std::vector<struct sockaddr_in> rx_addr(rx_sessions);
  std::vector<struct sockaddr_in> tx_bind_addr(tx_sessions); /* for dual loop */
  std::vector<struct sockaddr_in> rx_bind_addr(rx_sessions);
  int ret;
  int epoll_fd = -1;
  int sfd = -1;

  char* send_buf = new char[udp_len];
  char* recv_buf = new char[udp_len];
  int payload_len = udp_len - SHA256_DIGEST_LENGTH;
  ssize_t send;
  ssize_t recv;
  unsigned char sha_result[SHA256_DIGEST_LENGTH];

  for (int i = 0; i < tx_sessions; i++) {
    tx_fds[i] = -1;
    uint16_t fd_udp_port = udp_port + i;
    if (para->reuse_port) fd_udp_port = udp_port;
    if (para->mcast) {
      uplt_init_sockaddr(&tx_addr[i], ctx->mcast_ip_addr, fd_udp_port);
      uplt_init_sockaddr_any(&tx_bind_addr[i], fd_udp_port);
    } else {
      uplt_init_sockaddr(&tx_addr[i], ctx->sip_addr[UPLT_PORT_P], fd_udp_port);
      uplt_init_sockaddr(&tx_bind_addr[i], ctx->sip_addr[UPLT_PORT_P], fd_udp_port);
    }
  }
  for (int i = 0; i < rx_sessions; i++) {
    rx_fds[i] = -1;
    rx_timeout[i] = 0;
    rx_pkts[i] = 0;
    uint16_t fd_udp_port = udp_port + i;
    if (para->reuse_port) fd_udp_port = udp_port;

    if (para->mcast) {
      uplt_init_sockaddr(&rx_addr[i], ctx->mcast_ip_addr, fd_udp_port);
      uplt_init_sockaddr_any(&rx_bind_addr[i], fd_udp_port);
    } else {
      uplt_init_sockaddr(&rx_addr[i], ctx->sip_addr[UPLT_PORT_R], fd_udp_port);
      uplt_init_sockaddr(&rx_bind_addr[i], ctx->sip_addr[UPLT_PORT_R], fd_udp_port);
    }
  }

  for (int i = 0; i < tx_sessions; i++) {
    ret = uplt_socket_port(AF_INET, SOCK_DGRAM, 0, UPLT_PORT_P);
    EXPECT_GE(ret, 0);
    if (ret < 0) goto exit;
    tx_fds[i] = ret;

    if (dual_loop) {
      ret = bind(tx_fds[i], (const struct sockaddr*)&tx_bind_addr[i],
                 sizeof(tx_bind_addr[i]));
      EXPECT_GE(ret, 0);
      if (ret < 0) goto exit;

      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = para->rx_timeout_us;
      ret = setsockopt(tx_fds[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      EXPECT_GE(ret, 0);
      if (ret < 0) goto exit;
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    ret = uplt_socket_port(AF_INET, SOCK_DGRAM, 0, UPLT_PORT_R);
    EXPECT_GE(ret, 0);
    if (ret < 0) goto exit;
    rx_fds[i] = ret;
    if (para->reuse_port) {
      int reuse = 1;
      ret = setsockopt(rx_fds[i], SOL_SOCKET, SO_REUSEPORT, (const void*)&reuse,
                       sizeof(reuse));
      EXPECT_GE(ret, 0);
      if (ret < 0) goto exit;
    }

    ret = bind(rx_fds[i], (const struct sockaddr*)&rx_bind_addr[i],
               sizeof(rx_bind_addr[i]));
    EXPECT_GE(ret, 0);
    if (ret < 0) goto exit;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = para->rx_timeout_us;
    ret = setsockopt(rx_fds[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    EXPECT_GE(ret, 0);
    if (ret < 0) goto exit;

    if (para->mcast) {
      struct ip_mreq mreq;
      memset(&mreq, 0, sizeof(mreq));
      /* multicast addr */
      mreq.imr_multiaddr.s_addr = rx_addr[i].sin_addr.s_addr;
      /* local nic src ip */
      memcpy(&mreq.imr_interface.s_addr, ctx->sip_addr[UPLT_PORT_P], UPLT_IP_ADDR_LEN);
      ret = setsockopt(rx_fds[i], IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
      EXPECT_GE(ret, 0);
      if (ret < 0) goto exit;
    }
  }

  if (para->mix_fd) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sfd = signalfd(-1, &set, SFD_NONBLOCK);
    EXPECT_GE(sfd, 0);
  }

  if (para->use_epoll) {
    epoll_fd = epoll_create1(0);
    EXPECT_GE(epoll_fd, 0);
    for (int i = 0; i < rx_sessions; i++) {
      struct epoll_event ev;
      ev.events = EPOLLIN;
      ev.data.fd = rx_fds[i];
      ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, rx_fds[i], &ev);
      EXPECT_GE(ret, 0);
    }
    if (sfd > 0) {
      struct epoll_event ev;
      ev.events = EPOLLIN;
      ev.data.fd = sfd;
      ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sfd, &ev);
      EXPECT_GE(ret, 0);
    }
  }

  for (int loop = 0; loop < para->tx_pkts; loop++) {
    /* tx */
    for (int i = 0; i < tx_sessions; i++) {
      st_test_rand_data((uint8_t*)send_buf, payload_len, 0);
      send_buf[0] = i;
      SHA256((unsigned char*)send_buf, payload_len,
             (unsigned char*)send_buf + payload_len);

      struct msghdr msg;
      memset(&msg, 0, sizeof(msg));
      if (para->reuse_port) { /* reuse test use same port */
        msg.msg_namelen = sizeof(rx_addr[0]);
        msg.msg_name = &rx_addr[0];
      } else {
        msg.msg_namelen = sizeof(rx_addr[i]);
        msg.msg_name = &rx_addr[i];
      }
      if (para->sendmsg_gso) {
        int gso_nb = 4;
        char* gso_buf = new char[udp_len * gso_nb];
        for (int gso = 0; gso < gso_nb; gso++) {
          memcpy(&gso_buf[gso * udp_len], send_buf, udp_len);
        }
        struct iovec iov;
        iov.iov_base = gso_buf;
        iov.iov_len = udp_len * gso_nb;
        msg.msg_iovlen = 1;
        msg.msg_iov = &iov;
        char msg_control[CMSG_SPACE(sizeof(uint16_t))];
        msg.msg_control = msg_control;
        msg.msg_controllen = sizeof(msg_control);
        struct cmsghdr* cmsg;
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = 17;  // SOL_UDP;
        cmsg->cmsg_type = 103;  // UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN(sizeof(uint16_t));
        uint16_t* val_p;
        val_p = (uint16_t*)CMSG_DATA(cmsg);
        *val_p = udp_len;
        dbg("%s, use gso sendmsg\n", __func__);
        send = sendmsg(tx_fds[i], &msg, 0);
        EXPECT_EQ((size_t)send, udp_len * gso_nb);
        delete[] gso_buf;
      } else if (para->sendmsg) {
        struct iovec iov;
        iov.iov_base = send_buf;
        iov.iov_len = udp_len;
        msg.msg_iovlen = 1;
        msg.msg_iov = &iov;
        dbg("%s, use sendmsg\n", __func__);
        send = sendmsg(tx_fds[i], &msg, 0);
        EXPECT_EQ((size_t)send, udp_len);
      } else {
        if (para->reuse_port) { /* reuse test use same port */
          send = sendto(tx_fds[i], send_buf, udp_len, 0,
                        (const struct sockaddr*)&rx_addr[0], sizeof(rx_addr[0]));
        } else {
          send = sendto(tx_fds[i], send_buf, udp_len, 0,
                        (const struct sockaddr*)&rx_addr[i], sizeof(rx_addr[i]));
        }
        EXPECT_EQ((size_t)send, udp_len);
      }
    }
    if (para->tx_sleep_us) st_usleep(para->tx_sleep_us);

    int poll_succ = 0;
    int poll_retry = 0;
    int max_retry = 10;

    if (para->use_poll) {
      while (poll_retry < max_retry) {
        struct pollfd* fds = new struct pollfd[rx_sessions + 1];
        memset(fds, 0, sizeof(*fds) * (rx_sessions + 1));
        for (int i = 0; i < rx_sessions; i++) {
          fds[i].fd = rx_fds[i];
          fds[i].events = POLLIN;
        }
        if (sfd > 0) {
          fds[rx_sessions].fd = sfd;
          fds[rx_sessions].events = POLLIN;
          ret = poll(fds, rx_sessions + 1, para->rx_timeout_us / 1000);
        } else {
          ret = poll(fds, rx_sessions, para->rx_timeout_us / 1000);
        }
        EXPECT_GE(ret, 0);
        poll_succ = 0;
        for (int i = 0; i < rx_sessions; i++) {
          if (fds[i].revents) poll_succ++;
        }
        dbg("%s, poll %d succ on sessions %d on %d\n", __func__, poll_succ, rx_sessions,
            poll_retry);
        if (poll_succ >= rx_sessions) break;

        poll_retry++;
        st_usleep(1000);
        delete[] fds;
      }
    } else if (para->use_select) {
      while (poll_retry < max_retry) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int nfds = 0;
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = para->rx_timeout_us;
        for (int i = 0; i < rx_sessions; i++) {
          FD_SET(rx_fds[i], &readfds);
          if (rx_fds[i] > nfds) nfds = rx_fds[i];
          dbg("%s, i %d fd %d\n", __func__, i, rx_fds[i]);
        }
        if (sfd > 0) {
          FD_SET(sfd, &readfds);
          if (sfd > nfds) nfds = sfd;
        }
        nfds += 1; /*  highest-numbered fd plus 1 */
        ret = select(nfds, &readfds, NULL, NULL, &timeout);
        EXPECT_GE(ret, 0);
        dbg("%s, ret %d nfds %d\n", __func__, ret, nfds);
        poll_succ = 0;
        for (int i = 0; i < rx_sessions; i++) {
          if (FD_ISSET(rx_fds[i], &readfds)) poll_succ++;
        }
        dbg("%s, select %d succ on sessions %d on %d\n", __func__, poll_succ, rx_sessions,
            poll_retry);
        if (poll_succ >= rx_sessions) break;

        poll_retry++;
        st_usleep(1000);
      }
    } else if (para->use_epoll) {
      while (poll_retry < max_retry) {
        struct epoll_event* events = new struct epoll_event[rx_sessions];
        ret = epoll_wait(epoll_fd, events, rx_sessions, para->rx_timeout_us / 1000);
        EXPECT_GE(ret, 0);
        poll_succ = ret;
        dbg("%s, ret %d\n", __func__, ret);
        if (poll_succ >= rx_sessions) break;
        poll_retry++;
        st_usleep(1000);
      }
    }

    if (para->use_poll || para->use_select || para->use_epoll) {
      /* expect 50% succ at least */
      EXPECT_GT(poll_succ, rx_sessions / 2);
      dbg("%s, %d succ on sessions %d\n", __func__, poll_succ, rx_sessions);
    }

    for (int i = 0; i < rx_sessions; i++) {
      /* rx */
    session_rx:
      if (para->recvmsg) {
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        struct iovec iov;
        iov.iov_base = recv_buf;
        iov.iov_len = udp_len;
        msg.msg_iovlen = 1;
        msg.msg_iov = &iov;
        recv = recvmsg(rx_fds[i], &msg, 0);
      } else {
        recv = recvfrom(rx_fds[i], recv_buf, udp_len, 0, NULL, NULL);
      }
      if (recv < 0) { /* timeout */
        if (!para->sendmsg_gso && !para->reuse_port) {
          rx_timeout[i]++;
          err("%s, recv fail at session %d pkt %d\n", __func__, i, loop);
        }
        continue;
      }
      EXPECT_EQ((size_t)recv, udp_len);
      if (!para->reuse_port) {
        /* check idx */
        EXPECT_EQ((char)i, recv_buf[0]);
      }
      /* check sha */
      SHA256((unsigned char*)recv_buf, payload_len, sha_result);
      ret = memcmp(recv_buf + payload_len, sha_result, SHA256_DIGEST_LENGTH);
      EXPECT_EQ(ret, 0);
      rx_pkts[i]++;
      // test_sha_dump("upd_loop_sha", sha_result);
      if (para->sendmsg_gso || para->reuse_port) goto session_rx;
    }

    if (dual_loop) {
      for (int i = 0; i < rx_sessions; i++) {
        st_test_rand_data((uint8_t*)send_buf, payload_len, 0);
        send_buf[0] = i;
        SHA256((unsigned char*)send_buf, payload_len,
               (unsigned char*)send_buf + payload_len);
        send = sendto(rx_fds[i], send_buf, udp_len, 0,
                      (const struct sockaddr*)&tx_addr[i], sizeof(tx_addr[i]));
        EXPECT_EQ((size_t)send, udp_len);
      }
      if (para->tx_sleep_us) st_usleep(para->tx_sleep_us);

      for (int i = 0; i < tx_sessions; i++) {
        recv = recvfrom(tx_fds[i], recv_buf, udp_len, 0, NULL, NULL);
        if (recv < 0) { /* timeout */
          rx_timeout[i]++;
          err("%s, back recv fail at session %d pkt %d\n", __func__, i, loop);
          continue;
        }
        EXPECT_EQ((size_t)recv, udp_len);
        /* check idx */
        EXPECT_EQ((char)i, recv_buf[0]);
        /* check sha */
        SHA256((unsigned char*)recv_buf, payload_len, sha_result);
        ret = memcmp(recv_buf + payload_len, sha_result, SHA256_DIGEST_LENGTH);
        EXPECT_EQ(ret, 0);
      }
    }
  }

  /* rx timeout and pkts check */
  for (int i = 0; i < rx_sessions; i++) {
    EXPECT_LT(rx_timeout[i], para->max_rx_timeout_pkts);
    dbg("%s, recv at session %d pkts %d\n", __func__, i, rx_pkts[i]);
    EXPECT_GE(rx_pkts[i], para->tx_pkts);
    total_rx_pkts += rx_pkts[i];
  }
  if (para->reuse_port) {
    info("%s, total_rx_pkts %d for reuse test\n", __func__, total_rx_pkts);
    /* leave some space for miss since we disable the rx timeout for reuse test
     */
    EXPECT_GT(total_rx_pkts, (para->tx_pkts - 1) * tx_sessions);
  }

exit:
  if (epoll_fd > 0) {
    close(epoll_fd);
    epoll_fd = -1;
  }
  if (sfd > 0) {
    close(sfd);
    sfd = -1;
  }
  for (int i = 0; i < tx_sessions; i++) {
    if (tx_fds[i] > 0) close(tx_fds[i]);
  }
  for (int i = 0; i < rx_sessions; i++) {
    if (rx_fds[i] > 0) {
      if (para->mcast) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        /* multicast addr */
        mreq.imr_multiaddr.s_addr = rx_addr[i].sin_addr.s_addr;
        /* local nic src ip */
        memcpy(&mreq.imr_interface.s_addr, ctx->sip_addr[UPLT_PORT_P], UPLT_IP_ADDR_LEN);
        ret = setsockopt(rx_fds[i], IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        EXPECT_GE(ret, 0);
      }
      close(rx_fds[i]);
    }
  }
  delete[] send_buf;
  delete[] recv_buf;
  return 0;
}

TEST(Loop, single) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  loop_sanity_test(ctx, &para);
}

TEST(Loop, poll_multi_no_sleep) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.use_poll = true;
  para.sessions = 10;
  para.tx_sleep_us = 0;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, poll_multi_mix_fd) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.use_poll = true;
  para.sessions = 10;
  para.tx_sleep_us = 0;
  para.mix_fd = true;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, dual_single) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.dual_loop = true;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, dual_multi_no_sleep) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.dual_loop = true;
  para.sessions = 10;
  para.tx_sleep_us = 0;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, mcast_multi) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.mcast = true;
  para.sessions = 5;
  para.tx_sleep_us = 100;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, select_multi_no_sleep) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.use_select = true;
  para.sessions = 10;
  para.tx_sleep_us = 0;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, select_multi_mix_fd) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.use_select = true;
  para.sessions = 10;
  para.tx_sleep_us = 0;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, epoll_multi_no_sleep) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.use_epoll = true;
  para.sessions = 10;
  para.tx_sleep_us = 0;
  para.mix_fd = true;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, epoll_multi_mix_fd) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.use_epoll = true;
  para.sessions = 10;
  para.tx_sleep_us = 0;
  para.mix_fd = true;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, sendmsg_multi) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.use_epoll = true;
  para.sessions = 4;
  para.tx_sleep_us = 0;
  para.sendmsg = true;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, sendmsg_gso) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.use_epoll = true;
  para.sessions = 4;
  para.tx_sleep_us = 0;
  para.sendmsg = true;
  para.sendmsg_gso = true;
  para.rx_timeout_us = 0;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, recvmsg_multi) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.use_epoll = true;
  para.sessions = 4;
  para.tx_sleep_us = 0;
  para.recvmsg = true;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, reuse_port) {
  struct uplt_ctx* ctx = uplt_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.reuse_port = true;
  para.reuse_tx_sessions = 28;
  para.sessions = 4;
  para.rx_timeout_us = 0;
  loop_sanity_test(ctx, &para);
}