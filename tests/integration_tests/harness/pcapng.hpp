/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <ios>

namespace mtl_test {

namespace detail {

inline uint32_t pcapng_read_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace detail

/* Verify PCAPNG structure (SHB, block framing, and minimum non-empty EPB count). */
inline void verify_pcapng_file(const char* path, uint32_t min_epb_packets) {
  ASSERT_NE(path, nullptr);
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  ASSERT_TRUE(file.is_open()) << "failed to open pcapng file: " << path;
  const std::streamoff size_off = file.tellg();
  ASSERT_GE(size_off, static_cast<std::streamoff>(28))
      << "pcapng file too small: " << path;
  const uint64_t file_size = static_cast<uint64_t>(size_off);

  uint64_t off = 0;
  uint32_t block_idx = 0;
  uint32_t epb_cnt = 0;
  while (off < file_size) {
    ASSERT_LE(static_cast<uint64_t>(12u), file_size - off)
        << "truncated pcapng block header at offset " << off;
    uint8_t hdr[8];
    file.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    ASSERT_TRUE(static_cast<bool>(file.read(reinterpret_cast<char*>(hdr),
                                            static_cast<std::streamsize>(sizeof(hdr)))));
    const uint32_t block_type = detail::pcapng_read_le32(hdr);
    const uint32_t block_len = detail::pcapng_read_le32(hdr + 4);

    if (block_idx == 0) {
      ASSERT_EQ(block_type, 0x0A0D0D0Au) << "first pcapng block is not SHB: " << path;
    }
    ASSERT_GE(block_len, 12u) << "invalid pcapng block length at offset " << off;
    ASSERT_EQ(block_len % 4u, 0u) << "unaligned pcapng block length at offset " << off;
    ASSERT_LE(static_cast<uint64_t>(block_len), file_size - off)
        << "pcapng block exceeds file size at offset " << off;

    uint8_t trail[4];
    file.seekg(static_cast<std::streamoff>(off + block_len - 4u), std::ios::beg);
    ASSERT_TRUE(static_cast<bool>(file.read(
        reinterpret_cast<char*>(trail), static_cast<std::streamsize>(sizeof(trail)))));
    ASSERT_EQ(detail::pcapng_read_le32(trail), block_len)
        << "pcapng trailing length mismatch at offset " << off;

    if (block_type == 0x00000006u) {
      ASSERT_GE(block_len, 32u) << "truncated pcapng EPB at offset " << off;
      uint8_t len_buf[8];
      file.seekg(static_cast<std::streamoff>(off + 20u), std::ios::beg);
      ASSERT_TRUE(
          static_cast<bool>(file.read(reinterpret_cast<char*>(len_buf),
                                      static_cast<std::streamsize>(sizeof(len_buf)))));
      const uint32_t cap_len = detail::pcapng_read_le32(len_buf);
      const uint32_t orig_len = detail::pcapng_read_le32(len_buf + 4);
      EXPECT_GT(cap_len, 0u) << "empty pcapng EPB at offset " << off;
      EXPECT_EQ(cap_len, orig_len) << "truncated pcapng packet at offset " << off;
      ASSERT_LE(static_cast<uint64_t>(32u) + cap_len, block_len)
          << "pcapng EPB payload exceeds block length at offset " << off;
      if (cap_len > 0u) epb_cnt++;
    }
    off += block_len;
    block_idx++;
  }
  EXPECT_GE(epb_cnt, min_epb_packets) << "pcapng EPB packet count too low: " << path;
}

}  // namespace mtl_test
