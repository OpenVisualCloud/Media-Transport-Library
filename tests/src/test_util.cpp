/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "test_util.h"

#include <inttypes.h>

#include <chrono>
#include <ctime>
#include <fstream>

#include "log.h"

void test_sha_dump(const char* tag, unsigned char* sha) {
  for (size_t i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    err("0x%02x ", sha[i]);
  }
  err(", %s done\n", tag);
}

int st_test_check_patter(uint8_t* p, size_t sz, uint8_t base) {
  for (size_t i = 0; i < sz; i++) {
    if (p[i] != ((base + i) & 0xFF)) {
      err("%s, fail data 0x%x on %" PRIu64 " base 0x%x\n", __func__, p[i], i, base);
      return -EIO;
    }
  }

  return 0;
}

int st_test_cmp(uint8_t* s1, uint8_t* s2, size_t sz) {
  for (size_t i = 0; i < sz; i++) {
    if (s1[i] != s2[i]) {
      err("%s, mismatch on %" PRIu64 ", 0x%x 0x%x\n", __func__, i, s1[i], s2[i]);
      return -EIO;
    }
  }

  return 0;
}

int st_test_cmp_u16(uint16_t* s1, uint16_t* s2, size_t sz) {
  for (size_t i = 0; i < sz; i++) {
    if (s1[i] != s2[i]) {
      err("%s, mismatch on %" PRIu64 "\n", __func__, i);
      return -EIO;
    }
  }

  return 0;
}

void test_frame_dump(void* addr, size_t size, const char* prefix) {
  // Get current time
  auto now = std::chrono::system_clock::now();
  auto now_c = std::chrono::system_clock::to_time_t(now);

  // Open file with name prefix_<time>
  std::string filename = std::string(prefix) + "_" + std::to_string(now_c) + ".bin";
  std::ofstream file(filename, std::ios::out | std::ios::binary);

  // Write frame to file
  file.write(reinterpret_cast<const char*>(addr), size);

  // Close file
  file.close();
}