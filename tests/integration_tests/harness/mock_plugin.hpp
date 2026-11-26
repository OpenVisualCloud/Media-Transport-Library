/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

#include "log.h"
#include "tests.hpp"

namespace mtl_test {

/*
 * Unified worker-thread mock plugin pool for ST 20 converter, ST 22 encoder,
 * and ST 22 decoder integration tests.
 *
 * Replaces three near-identical pthread/calloc/condvar implementations across
 * st20p_test.cpp and st22p_test.cpp with one type-safe C++ template.
 *
 * Traits must provide:
 *   using SessionHandle = ...;  // e.g. st20p_convert_session
 *   using PrivHandle    = ...;  // e.g. st20_convert_priv
 *   using CreateReq     = ...;  // e.g. struct st20_converter_create_req
 *   using FrameMeta     = ...;  // e.g. struct st20_convert_frame_meta
 *   static constexpr size_t kMaxSessions = 8;
 *   static bool use_block_get(struct st_tests_context* ctx);
 *   static void on_create_req(struct st_tests_context* ctx, CreateReq* req,
 *                             int& sleep_time_us);
 *   static void set_block_timeout(SessionHandle s, uint64_t ns);
 *   static void wake_block(SessionHandle s);
 *   static FrameMeta* get_frame(SessionHandle s);
 *   static int put_frame(SessionHandle s, FrameMeta* f, int result);
 *   static int process_frame(const CreateReq& req, FrameMeta* f, int sleep_time_us,
 *                            int rand_ratio, int& frame_cnt);
 */
template <typename Traits>
class MockPluginPool {
 public:
  struct Session {
    int idx = -1;
    struct st_tests_context* ctx = nullptr;
    typename Traits::CreateReq req = {};
    typename Traits::SessionHandle session_p = nullptr;
    bool block_get = false;
    int sleep_time_us = 0;
    int frame_cnt = 0;
    int fail_interval = 0;
    int timeout_interval = 0;
    int timeout_ms = 0;
    int rand_ratio = 0;

    std::atomic<bool> stop{false};
    std::mutex wake_mtx;
    std::condition_variable wake_cv;
    bool woken = false;
    std::thread worker;

    ~Session() {
      stop_and_join();
    }

    void stop_and_join() {
      {
        std::lock_guard<std::mutex> lk(wake_mtx);
        stop.store(true, std::memory_order_release);
        woken = true;
      }
      if (!worker.joinable()) return;
      wake_cv.notify_all();
      if (block_get && session_p) Traits::wake_block(session_p);
      worker.join();
    }
  };

  static MockPluginPool& instance() {
    static MockPluginPool pool;
    return pool;
  }

  static typename Traits::PrivHandle create_session(
      void* priv, typename Traits::SessionHandle session_p,
      typename Traits::CreateReq* req) {
    auto* ctx = static_cast<struct st_tests_context*>(priv);
    auto& pool = instance();
    std::lock_guard<std::mutex> lock(pool.slots_mtx_);

    for (size_t i = 0; i < Traits::kMaxSessions; i++) {
      if (pool.slots_[i]) continue;

      auto s = std::make_unique<Session>();
      s->idx = static_cast<int>(i);
      s->ctx = ctx;
      s->session_p = session_p;
      s->block_get = Traits::use_block_get(ctx);
      Traits::on_create_req(ctx, req, s->sleep_time_us);
      s->req = *req;
      s->fail_interval = ctx->plugin_fail_interval;
      s->timeout_interval = ctx->plugin_timeout_interval;
      s->timeout_ms = ctx->plugin_timeout_ms;
      s->rand_ratio = ctx->plugin_rand_ratio;

      Session* raw = s.get();
      s->worker = std::thread([raw]() { run_worker(raw); });
      pool.slots_[i] = std::move(s);
      dbg("%s(%d), input fmt: %s, output fmt: %s\n", __func__, raw->idx,
          st_frame_fmt_name(req->input_fmt), st_frame_fmt_name(req->output_fmt));
      return static_cast<typename Traits::PrivHandle>(raw);
    }

    dbg("%s, all session slots are used\n", __func__);
    return nullptr;
  }

  static int free_session(void* priv, typename Traits::PrivHandle session) {
    (void)priv;
    auto* s = static_cast<Session*>(session);
    if (!s) return 0;

    s->stop_and_join();
    dbg("%s(%d), total %d frames\n", __func__, s->idx, s->frame_cnt);

    auto& pool = instance();
    std::lock_guard<std::mutex> lock(pool.slots_mtx_);
    if (s->idx >= 0 && static_cast<size_t>(s->idx) < Traits::kMaxSessions &&
        pool.slots_[static_cast<size_t>(s->idx)].get() == s) {
      pool.slots_[static_cast<size_t>(s->idx)].reset();
    }
    return 0;
  }

  static int notify_frame_available(void* priv) {
    auto* s = static_cast<Session*>(priv);
    if (!s || s->block_get) return 0;
    std::lock_guard<std::mutex> lk(s->wake_mtx);
    s->woken = true;
    s->wake_cv.notify_one();
    return 0;
  }

 private:
  static void run_worker(Session* s) {
    if (s->block_get) Traits::set_block_timeout(s->session_p, NS_PER_S);

    dbg("%s(%d), start\n", __func__, s->idx);
    while (!s->stop.load(std::memory_order_acquire)) {
      auto* frame = Traits::get_frame(s->session_p);
      if (!frame) {
        if (!s->block_get) {
          std::unique_lock<std::mutex> lk(s->wake_mtx);
          s->wake_cv.wait(
              lk, [s]() { return s->woken || s->stop.load(std::memory_order_acquire); });
          s->woken = false;
        }
        continue;
      }

      int result = Traits::process_frame(s->req, frame, s->sleep_time_us, s->rand_ratio,
                                         s->frame_cnt);
      if (result == 0) {
        if (s->fail_interval > 0 && !(s->frame_cnt % s->fail_interval)) {
          result = -EIO;
        } else if (s->timeout_interval > 0 && s->timeout_ms > 0 &&
                   !(s->frame_cnt % s->timeout_interval)) {
          std::unique_lock<std::mutex> lk(s->wake_mtx);
          s->wake_cv.wait_for(lk, std::chrono::milliseconds(s->timeout_ms),
                              [s]() { return s->stop.load(std::memory_order_acquire); });
        }
      }
      Traits::put_frame(s->session_p, frame, result);
    }
    dbg("%s(%d), stop\n", __func__, s->idx);
  }

  std::mutex slots_mtx_;
  std::unique_ptr<Session> slots_[Traits::kMaxSessions];
};

}  // namespace mtl_test
