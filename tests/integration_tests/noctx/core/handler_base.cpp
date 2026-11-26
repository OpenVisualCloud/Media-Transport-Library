/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "handler_base.hpp"

Handlers::Handlers(st_tests_context* ctx, FrameTestStrategy* frameTestStrategy)
    : ctx(ctx), frameTestStrategy(frameTestStrategy) {
}

void Handlers::stopSession() {
  session.stop();
}
