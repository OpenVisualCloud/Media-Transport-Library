/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C shim for mt_bitmap_test.cpp: mt_util.h pulls in mt_main.h, which is C only.
 * Going through this file makes the compiler check the calls against the real
 * header, so a change of the signature breaks the test build.
 */

#include "mt_util.h"

bool ut_bitmap_test(uint8_t* bitmap, size_t size, int idx) {
  return mt_bitmap_test(bitmap, size, idx);
}

bool ut_bitmap_test_and_set(uint8_t* bitmap, size_t size, int idx) {
  return mt_bitmap_test_and_set(bitmap, size, idx);
}

bool ut_bitmap_test_and_unset(uint8_t* bitmap, size_t size, int idx) {
  return mt_bitmap_test_and_unset(bitmap, size, idx);
}
