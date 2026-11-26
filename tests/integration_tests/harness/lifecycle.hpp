/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "harness/sessions.hpp"
#include "log.h"

namespace mtl_test {

namespace detail {

template <typename Ops, typename = void>
struct has_port_member : std::false_type {};

template <typename Ops>
struct has_port_member<Ops, std::void_t<decltype(std::declval<Ops&>().port.udp_port)>>
    : std::true_type {};

template <typename Ops, typename = void>
struct has_rtp_frame_total_pkts : std::false_type {};

template <typename Ops>
struct has_rtp_frame_total_pkts<
    Ops, std::void_t<decltype(std::declval<Ops&>().rtp_frame_total_pkts)>>
    : std::true_type {};

template <typename Ops>
inline void bump_udp_ports(Ops& ops) {
  if constexpr (has_port_member<Ops>::value) {
    ops.port.udp_port[MTL_SESSION_PORT_P]++;
    ops.port.udp_port[MTL_SESSION_PORT_R]++;
  } else {
    ops.udp_port[MTL_SESSION_PORT_P]++;
    ops.udp_port[MTL_SESSION_PORT_R]++;
  }
}

template <typename Ops>
inline void set_num_port(Ops& ops, int8_t num_port) {
  if constexpr (has_port_member<Ops>::value) {
    ops.port.num_port = num_port;
  } else {
    ops.num_port = num_port;
  }
}

}  // namespace detail

/*
 * Verify repeated session create/free cycles with RAII cleanup (zero handle or
 * context leaks on assertion failure and no artificial inter-session sleeps).
 */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn, typename AssertCntFn>
inline void verify_create_free(OpsInitFn&& ops_init, AssertCntFn&& assert_cnt, int base,
                               int step, int repeat) {
  ASSERT_GE(base, 0);
  ASSERT_GE(step, 0);
  ASSERT_GE(repeat, 0);
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;
  auto test_ctx = make_context(ctx, 0, 2);
  ASSERT_TRUE(test_ctx != nullptr);

  Ops ops;
  ops_init(test_ctx.get(), &ops);

  int expect_cnt = 0;
  std::vector<ScopedHandle<Handle, FreeFn>> base_handles;
  base_handles.reserve(static_cast<size_t>(base));
  for (int i = 0; i < base; i++) {
    ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
    ASSERT_TRUE(static_cast<bool>(h));
    base_handles.push_back(std::move(h));
    detail::bump_udp_ports(ops);
    expect_cnt++;
    assert_cnt(expect_cnt);
  }

  for (int r = 0; r < repeat; r++) {
    std::vector<ScopedHandle<Handle, FreeFn>> step_handles;
    step_handles.reserve(static_cast<size_t>(step));
    for (int j = 0; j < step; j++) {
      ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
      ASSERT_TRUE(static_cast<bool>(h));
      step_handles.push_back(std::move(h));
      detail::bump_udp_ports(ops);
      expect_cnt++;
      assert_cnt(expect_cnt);
    }

    for (auto& h : step_handles) {
      h.reset();
      expect_cnt--;
      assert_cnt(expect_cnt);
    }
  }

  for (auto& h : base_handles) {
    h.reset();
    expect_cnt--;
    assert_cnt(expect_cnt);
  }

  assert_cnt(0);
}

/* Verify allocating up to max_sessions and releasing all returns scheduler count. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn, typename AssertCntFn>
inline void verify_create_free_max(OpsInitFn&& ops_init, AssertCntFn&& assert_cnt,
                                   int max_sessions) {
  ASSERT_GT(max_sessions, 0);
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;
  auto test_ctx = make_context(ctx, 0, 2);
  ASSERT_TRUE(test_ctx != nullptr);
  const int sch_cnt = st_test_sch_cnt(ctx);

  Ops ops;
  ops_init(test_ctx.get(), &ops);

  int expect_cnt = 0;
  std::vector<ScopedHandle<Handle, FreeFn>> handles;
  handles.reserve(static_cast<size_t>(max_sessions));
  for (int i = 0; i < max_sessions; i++) {
    ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
    if (!h) break;
    handles.push_back(std::move(h));
    detail::bump_udp_ports(ops);
    expect_cnt++;
    assert_cnt(expect_cnt);
  }

  info("%s, max session cnt %d\n", __func__, expect_cnt);
  EXPECT_GT(expect_cnt, 0);
  for (auto& h : handles) {
    h.reset();
    expect_cnt--;
    assert_cnt(expect_cnt);
  }

  assert_cnt(0);
  EXPECT_EQ(sch_cnt, st_test_sch_cnt(ctx));
}

/* Verify invalid num_port values (0, 100, -1, and 2 on single-port hosts) fail. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn>
inline void verify_invalid_ports(OpsInitFn&& ops_init) {
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;
  auto test_ctx = make_context(ctx, 0, 2);
  ASSERT_TRUE(test_ctx != nullptr);

  Ops ops;
  ops_init(test_ctx.get(), &ops);

  for (int8_t bad_ports :
       {static_cast<int8_t>(0), static_cast<int8_t>(100), static_cast<int8_t>(-1)}) {
    detail::set_num_port(ops, bad_ports);
    ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
    EXPECT_FALSE(static_cast<bool>(h));
  }

  if (ctx->para.num_ports < 2) {
    detail::set_num_port(ops, 2);
    ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
    EXPECT_FALSE(static_cast<bool>(h));
  }
}

/* Verify creating a session with an invalid framebuffer count fails cleanly. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn>
inline void verify_invalid_fb_cnt(OpsInitFn&& ops_init, uint16_t fb_nb) {
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;
  auto test_ctx = make_context(ctx, 0, fb_nb);
  ASSERT_TRUE(test_ctx != nullptr);

  Ops ops;
  ops_init(test_ctx.get(), &ops);
  detail::set_num_port(ops, 1);
  ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
  EXPECT_FALSE(static_cast<bool>(h));
}

/* Verify every in-range framebuffer index returns a non-null pointer. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename GetFbFn, typename OpsInitFn>
inline void verify_get_framebuffer(OpsInitFn&& ops_init, GetFbFn&& get_fb,
                                   uint16_t fb_nb) {
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;
  auto test_ctx = make_context(ctx, 0, fb_nb);
  ASSERT_TRUE(test_ctx != nullptr);

  Ops ops;
  ops_init(test_ctx.get(), &ops);
  detail::set_num_port(ops, 1);
  ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
  ASSERT_TRUE(static_cast<bool>(h));

  for (uint16_t idx = 0; idx < fb_nb; idx++) {
    EXPECT_NE(get_fb(h.get(), idx), nullptr);
  }
}

/* Verify out-of-range framebuffer indices return null. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename GetFbFn, typename OpsInitFn>
inline void verify_get_framebuffer_oob(OpsInitFn&& ops_init, GetFbFn&& get_fb,
                                       uint16_t fb_nb) {
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;
  auto test_ctx = make_context(ctx, 0, fb_nb);
  ASSERT_TRUE(test_ctx != nullptr);

  Ops ops;
  ops_init(test_ctx.get(), &ops);
  detail::set_num_port(ops, 1);
  ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
  ASSERT_TRUE(static_cast<bool>(h));

  EXPECT_EQ(get_fb(h.get(), fb_nb), nullptr);
  EXPECT_EQ(get_fb(h.get(), static_cast<uint16_t>(fb_nb * 2)), nullptr);
}

/* Verify invalid rtp_ring_size values fail for sessions with explicit ops.type. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename SessionType, typename OpsInitFn>
inline void verify_rtp_ring_invalid(OpsInitFn&& ops_init, SessionType s_type,
                                    uint16_t ring_sz) {
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;
  auto test_ctx = make_context(ctx, 0, 0);
  ASSERT_TRUE(test_ctx != nullptr);

  Ops ops;
  ops_init(test_ctx.get(), &ops);
  detail::set_num_port(ops, 1);
  ops.type = s_type;
  ops.rtp_ring_size = ring_sz;
  ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
  EXPECT_FALSE(static_cast<bool>(h));
}

/* Verify invalid rtp_ring_size values fail for sessions without ops.type mutation. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn>
inline void verify_rtp_ring_invalid(OpsInitFn&& ops_init, uint16_t ring_sz) {
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;
  auto test_ctx = make_context(ctx, 0, 0);
  ASSERT_TRUE(test_ctx != nullptr);

  Ops ops;
  ops_init(test_ctx.get(), &ops);
  detail::set_num_port(ops, 1);
  ops.rtp_ring_size = ring_sz;
  ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
  EXPECT_FALSE(static_cast<bool>(h));
}

/* Verify rtp_pkt_size validation (with optional rtp_frame_total_pkts initialization). */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename SessionType, typename OpsInitFn>
inline void verify_rtp_pkt_size(OpsInitFn&& ops_init, SessionType s_type, uint16_t pkt_sz,
                                bool expect_ok, bool set_total_pkts = false) {
  auto* ctx = st_test_ctx();
  mtl_handle m_handle = ctx->handle;
  auto test_ctx = make_context(ctx, 0, 0);
  ASSERT_TRUE(test_ctx != nullptr);

  Ops ops;
  ops_init(test_ctx.get(), &ops);
  detail::set_num_port(ops, 1);
  ops.type = s_type;
  ops.rtp_ring_size = 1024;
  if constexpr (detail::has_rtp_frame_total_pkts<Ops>::value) {
    if (set_total_pkts) {
      ops.rtp_frame_total_pkts = 1024;
    }
  } else {
    (void)set_total_pkts;
  }
  ops.rtp_pkt_size = pkt_sz;

  ScopedHandle<Handle, FreeFn> h(CreateFn(m_handle, &ops));
  EXPECT_EQ(static_cast<bool>(h), expect_ok);
}

/* Overload for ops structs without rtp_frame_total_pkts (e.g., st30/st40 RX). */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename SessionType, typename OpsInitFn>
inline void verify_rtp_pkt_size_no_total(OpsInitFn&& ops_init, SessionType s_type,
                                         uint16_t pkt_sz, bool expect_ok) {
  verify_rtp_pkt_size<Ops, Handle, CreateFn, FreeFn>(std::forward<OpsInitFn>(ops_init),
                                                     s_type, pkt_sz, expect_ok, false);
}

}  // namespace mtl_test
