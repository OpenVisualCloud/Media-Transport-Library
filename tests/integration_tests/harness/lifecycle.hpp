/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <cstdint>
#include <set>
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

template <typename Ops, typename = void>
struct has_rtp_pkt_size : std::false_type {};

template <typename Ops>
struct has_rtp_pkt_size<Ops, std::void_t<decltype(std::declval<Ops&>().rtp_pkt_size)>>
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

template <typename Ops, typename OpsInitFn, typename BodyFn>
inline void with_session_ops(OpsInitFn&& ops_init, uint16_t fb_nb, bool single_port,
                             BodyFn&& body) {
  auto* ctx = st_test_ctx();
  auto test_ctx = make_context(ctx, 0, fb_nb);
  ASSERT_TRUE(test_ctx != nullptr);
  Ops ops;
  ops_init(test_ctx.get(), &ops);
  if (single_port) set_num_port(ops, 1);
  body(ctx, ops);
}

template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename GetFbFn, typename OpsInitFn>
inline void verify_framebuffer_access(OpsInitFn&& ops_init, GetFbFn&& get_fb,
                                      uint16_t fb_nb, bool check_oob) {
  with_session_ops<Ops>(std::forward<OpsInitFn>(ops_init), fb_nb, true,
                        [&](struct st_tests_context* ctx, Ops& ops) {
                          ScopedHandle<Handle, FreeFn> h(CreateFn(ctx->handle, &ops));
                          ASSERT_TRUE(static_cast<bool>(h));
                          if (check_oob) {
                            EXPECT_EQ(get_fb(h.get(), fb_nb), nullptr);
                            EXPECT_EQ(get_fb(h.get(), static_cast<uint16_t>(fb_nb * 2)),
                                      nullptr);
                          } else {
                            std::set<const void*> ptrs;
                            for (uint16_t idx = 0; idx < fb_nb; idx++) {
                              const void* fb = get_fb(h.get(), idx);
                              EXPECT_NE(fb, nullptr);
                              if (fb) ptrs.insert(fb);
                            }
                            EXPECT_EQ(ptrs.size(), static_cast<size_t>(fb_nb));
                          }
                        });
}

}  // namespace detail

/* Verify repeated session create/free cycles with RAII cleanup. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn, typename AssertCntFn>
inline void verify_create_free(OpsInitFn&& ops_init, AssertCntFn&& assert_cnt, int base,
                               int step, int repeat) {
  ASSERT_GE(base, 0);
  ASSERT_GE(step, 0);
  ASSERT_GE(repeat, 0);
  detail::with_session_ops<Ops>(
      std::forward<OpsInitFn>(ops_init), 2, false,
      [&](struct st_tests_context* ctx, Ops& ops) {
        const int base_sch = st_test_sch_cnt(ctx);
        int expect_cnt = 0;
        auto create_batch = [&](int n) {
          std::vector<ScopedHandle<Handle, FreeFn>> batch;
          batch.reserve(static_cast<size_t>(n));
          for (int i = 0; i < n; i++) {
            ScopedHandle<Handle, FreeFn> h(CreateFn(ctx->handle, &ops));
            EXPECT_TRUE(static_cast<bool>(h));
            if (!h) return batch;
            batch.push_back(std::move(h));
            detail::bump_udp_ports(ops);
            assert_cnt(++expect_cnt);
          }
          return batch;
        };

        auto base_handles = create_batch(base);
        ASSERT_EQ(static_cast<int>(base_handles.size()), base);
        for (int r = 0; r < repeat; r++) {
          auto step_handles = create_batch(step);
          ASSERT_EQ(static_cast<int>(step_handles.size()), step);
          for (auto& h : step_handles) {
            h.reset();
            assert_cnt(--expect_cnt);
          }
        }
        for (auto& h : base_handles) {
          h.reset();
          assert_cnt(--expect_cnt);
        }
        assert_cnt(0);
        EXPECT_EQ(st_test_sch_cnt(ctx), base_sch);
      });
}

/* Verify allocating up to max_sessions and releasing all returns scheduler count. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn, typename AssertCntFn>
inline void verify_create_free_max(OpsInitFn&& ops_init, AssertCntFn&& assert_cnt,
                                   int max_sessions) {
  ASSERT_GT(max_sessions, 0);
  detail::with_session_ops<Ops>(
      std::forward<OpsInitFn>(ops_init), 2, false,
      [&](struct st_tests_context* ctx, Ops& ops) {
        const int sch_cnt = st_test_sch_cnt(ctx);
        int expect_cnt = 0;
        std::vector<ScopedHandle<Handle, FreeFn>> handles;
        handles.reserve(static_cast<size_t>(max_sessions));
        for (int i = 0; i < max_sessions; i++) {
          ScopedHandle<Handle, FreeFn> h(CreateFn(ctx->handle, &ops));
          if (!h) break;
          handles.push_back(std::move(h));
          detail::bump_udp_ports(ops);
          assert_cnt(++expect_cnt);
        }

        info("%s, max session cnt %d\n", __func__, expect_cnt);
        EXPECT_GE(expect_cnt, 2);
        for (auto& h : handles) {
          h.reset();
          assert_cnt(--expect_cnt);
        }
        assert_cnt(0);
        EXPECT_EQ(sch_cnt, st_test_sch_cnt(ctx));

        /* Prove session and queue resources were reclaimed after max teardown. */
        {
          ScopedHandle<Handle, FreeFn> reclaimed(CreateFn(ctx->handle, &ops));
          EXPECT_TRUE(static_cast<bool>(reclaimed));
          if (reclaimed) assert_cnt(1);
        }
        assert_cnt(0);
        EXPECT_EQ(sch_cnt, st_test_sch_cnt(ctx));
      });
}

/* Verify invalid num_port values (0, 3, 100, -1, and 2 on single-port hosts) fail. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn>
inline void verify_invalid_ports(OpsInitFn&& ops_init) {
  detail::with_session_ops<Ops>(
      std::forward<OpsInitFn>(ops_init), 2, false,
      [&](struct st_tests_context* ctx, Ops& ops) {
        for (int8_t bad_ports :
             {static_cast<int8_t>(0), static_cast<int8_t>(MTL_SESSION_PORT_MAX + 1),
              static_cast<int8_t>(100), static_cast<int8_t>(-1)}) {
          detail::set_num_port(ops, bad_ports);
          ScopedHandle<Handle, FreeFn> h(CreateFn(ctx->handle, &ops));
          EXPECT_FALSE(static_cast<bool>(h));
        }
        if (ctx->para.num_ports < 2) {
          detail::set_num_port(ops, 2);
          ScopedHandle<Handle, FreeFn> h(CreateFn(ctx->handle, &ops));
          EXPECT_FALSE(static_cast<bool>(h));
        }
      });
}

/* Verify creating a session with an invalid framebuffer count fails cleanly. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn>
inline void verify_invalid_fb_cnt(OpsInitFn&& ops_init, uint16_t fb_nb) {
  detail::with_session_ops<Ops>(
      std::forward<OpsInitFn>(ops_init), fb_nb, true,
      [&](struct st_tests_context* ctx, Ops& ops) {
        ScopedHandle<Handle, FreeFn> h(CreateFn(ctx->handle, &ops));
        EXPECT_FALSE(static_cast<bool>(h));
      });
}

/* Verify every in-range framebuffer index returns a distinct non-null pointer. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename GetFbFn, typename OpsInitFn>
inline void verify_get_framebuffer(OpsInitFn&& ops_init, GetFbFn&& get_fb,
                                   uint16_t fb_nb) {
  detail::verify_framebuffer_access<Ops, Handle, CreateFn, FreeFn>(
      std::forward<OpsInitFn>(ops_init), std::forward<GetFbFn>(get_fb), fb_nb, false);
}

/* Verify out-of-range framebuffer indices return null. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename GetFbFn, typename OpsInitFn>
inline void verify_get_framebuffer_oob(OpsInitFn&& ops_init, GetFbFn&& get_fb,
                                       uint16_t fb_nb) {
  detail::verify_framebuffer_access<Ops, Handle, CreateFn, FreeFn>(
      std::forward<OpsInitFn>(ops_init), std::forward<GetFbFn>(get_fb), fb_nb, true);
}

/* Verify invalid rtp_ring_size values fail after asserting a valid positive control. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename OpsInitFn>
inline void verify_rtp_ring_invalid(OpsInitFn&& ops_init, uint16_t ring_sz) {
  detail::with_session_ops<Ops>(
      std::forward<OpsInitFn>(ops_init), 0, true,
      [&](struct st_tests_context* ctx, Ops& ops) {
        if constexpr (detail::has_rtp_frame_total_pkts<Ops>::value) {
          if (ops.rtp_frame_total_pkts == 0) ops.rtp_frame_total_pkts = 1024;
        }
        if constexpr (detail::has_rtp_pkt_size<Ops>::value) {
          if (ops.rtp_pkt_size == 0) ops.rtp_pkt_size = 1260;
        }
        /* Positive control: valid power-of-two rtp_ring_size must succeed. */
        ops.rtp_ring_size = 128;
        {
          ScopedHandle<Handle, FreeFn> ok(CreateFn(ctx->handle, &ops));
          ASSERT_TRUE(static_cast<bool>(ok));
        }
        ops.rtp_ring_size = ring_sz;
        ScopedHandle<Handle, FreeFn> h(CreateFn(ctx->handle, &ops));
        EXPECT_FALSE(static_cast<bool>(h));
      });
}

/* Verify invalid rtp_ring_size values fail for sessions with explicit ops.type. */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename SessionType, typename OpsInitFn>
inline void verify_rtp_ring_invalid(OpsInitFn&& ops_init, SessionType s_type,
                                    uint16_t ring_sz) {
  verify_rtp_ring_invalid<Ops, Handle, CreateFn, FreeFn>(
      [&](tests_context* c, Ops* ops) {
        ops_init(c, ops);
        ops->type = s_type;
      },
      ring_sz);
}

/* Verify rtp_pkt_size validation (with optional rtp_frame_total_pkts initialization). */
template <typename Ops, typename Handle, Handle (*CreateFn)(mtl_handle, Ops*),
          int (*FreeFn)(Handle), typename SessionType, typename OpsInitFn>
inline void verify_rtp_pkt_size(OpsInitFn&& ops_init, SessionType s_type, uint16_t pkt_sz,
                                bool expect_ok, bool set_total_pkts = false) {
  detail::with_session_ops<Ops>(
      std::forward<OpsInitFn>(ops_init), 0, true,
      [&](struct st_tests_context* ctx, Ops& ops) {
        ops.type = s_type;
        ops.rtp_ring_size = 1024;
        if constexpr (detail::has_rtp_frame_total_pkts<Ops>::value) {
          if (set_total_pkts) ops.rtp_frame_total_pkts = 1024;
        } else {
          (void)set_total_pkts;
        }
        ops.rtp_pkt_size = pkt_sz;
        ScopedHandle<Handle, FreeFn> h(CreateFn(ctx->handle, &ops));
        EXPECT_EQ(static_cast<bool>(h), expect_ok);
      });
}

}  // namespace mtl_test
