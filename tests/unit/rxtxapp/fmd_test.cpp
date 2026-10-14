/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * RxTxApp in ST 2110-41 fast metadata mode: the json keys, the TX chunking of
 * st40p_url, the reference compare and the RX verdict of the st40p and the
 * ancillary sessions. See fmd_harness.c.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='RxTxAppFmd*'
 */

#include <dirent.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "rxtxapp/fmd_harness.h"
#include "st40_pipeline_api.h"

namespace {

constexpr uint32_t kDit = 1234567;
constexpr uint64_t kNsPerMs = 1000000;
/* p59: 1001/60000 s */
constexpr uint64_t kFrameNs = 16683333;

/* a data item as the lib delivers it, zero padded to a word */
using Item = std::vector<uint8_t>;

std::string write_file(const std::vector<uint8_t>& bytes) {
  char path[] = "/tmp/ut_rxtxapp_fmd_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) return "";
  if (!bytes.empty() && write(fd, bytes.data(), bytes.size()) != (ssize_t)bytes.size()) {
    close(fd);
    return "";
  }
  close(fd);
  return path;
}

std::vector<uint8_t> random_bytes(size_t n, uint32_t seed) {
  std::mt19937 gen(seed);
  std::vector<uint8_t> v(n);
  for (auto& b : v) b = (uint8_t)gen();
  return v;
}

int open_fds() {
  int n = 0;
  DIR* d = opendir("/proc/self/fd");
  if (!d) return -1;
  while (readdir(d)) n++;
  closedir(d);
  return n;
}

std::string session_json(const std::string& url, const char* url_key = "st40p_url") {
  std::string j = "{\"start_port\": 30000, \"payload_type\": 115, \"fps\": \"p59\"";
  if (!url.empty()) j += std::string(", \"") + url_key + "\": \"" + url + "\"";
  j += ", \"fast_metadata\": true, \"fastmetadata_data_item_type\": " +
       std::to_string(kDit) + ", \"fastmetadata_k_bit\": 0}";
  return j;
}

/* what an RX of the stream of TX gets */
std::vector<Item> tx_items(const std::string& url, int n) {
  std::vector<Item> items;
  ut_tx_st40p* tx = ut_tx_st40p_create(session_json(url).c_str());
  if (!tx) return items;
  int put = ut_tx_st40p_run(tx, n);
  for (int i = 0; i < put; i++) {
    uint8_t buf[2048];
    uint32_t len = ut_tx_st40p_item(tx, i, buf, sizeof(buf));
    items.emplace_back(buf, buf + len);
  }
  ut_tx_st40p_free(tx);
  return items;
}

}  // namespace

class RxTxAppFmdTest : public ::testing::Test {
 protected:
  std::vector<std::string> files_;
  int invalid_ = 0;

  void TearDown() override {
    for (auto& f : files_) unlink(f.c_str());
  }

  std::string file(const std::vector<uint8_t>& bytes) {
    std::string p = write_file(bytes);
    files_.push_back(p);
    return p;
  }

  /* feeds items at the p59 rate to an rx st40p session, returns the result */
  int rx_verdict(const std::string& rx_json, const std::vector<Item>& items,
                 std::string* line = nullptr, const std::vector<bool>& discont = {},
                 uint64_t tail_ns = kFrameNs) {
    ut_rx_st40p* rx = ut_rx_st40p_create(rx_json.c_str());
    EXPECT_NE(rx, nullptr);
    if (!rx) return -1;
    uint64_t t = 1000 * kNsPerMs;
    for (size_t i = 0; i < items.size(); i++) {
      bool d = i < discont.size() && discont[i];
      ut_rx_st40p_push(rx, items[i].data(), items[i].size(), kDit, 0, false, d, d ? 1 : 0,
                       t);
      t += kFrameNs;
    }
    ut_rx_st40p_run(rx);
    ut_rxtxapp_set_now(t - kFrameNs + tail_ns);
    testing::internal::CaptureStdout();
    int ret = ut_rx_st40p_result(rx);
    std::string out = testing::internal::GetCapturedStdout();
    if (line) *line = out;
    invalid_ = ut_rx_st40p_invalid(rx);
    ut_rx_st40p_free(rx);
    return ret;
  }

  /* feeds items at the p59 rate to an rx ancillary session, seqs are the RTP seq */
  int anc_verdict(const std::string& url, const std::vector<Item>& items,
                  const std::vector<uint16_t>& seqs) {
    ut_rx_anc* rx = ut_rx_anc_create(session_json(url, "ancillary_url").c_str());
    EXPECT_NE(rx, nullptr);
    if (!rx) return -1;
    uint64_t t = 1000 * kNsPerMs;
    for (size_t i = 0; i < items.size(); i++, t += kFrameNs)
      ut_rx_anc_push(rx, seqs[i], items[i].data(), items[i].size(), t);
    ut_rx_anc_run(rx);
    ut_rxtxapp_set_now(t);
    int ret = ut_rx_anc_result(rx);
    invalid_ = ut_rx_anc_invalid(rx);
    ut_rx_anc_free(rx);
    return ret;
  }
};

/* TX cuts st40p_url at ST_APP_FMD_CHUNK_BYTES, RX accepts every item of it */
class RxTxAppFmdLoopbackTest : public RxTxAppFmdTest,
                               public ::testing::WithParamInterface<size_t> {};

TEST_P(RxTxAppFmdLoopbackTest, GoodStreamIsOk) {
  std::vector<uint8_t> payload = random_bytes(GetParam(), (uint32_t)GetParam());
  std::string url = file(payload);
  std::vector<Item> items = tx_items(url, 300);
  ASSERT_EQ(items.size(), 300u);

  std::string line;
  EXPECT_EQ(rx_verdict(session_json(url), items, &line), 0) << line;
  EXPECT_EQ(invalid_, 0) << line;
  EXPECT_NE(line.find(", OK,"), std::string::npos) << line;
}

INSTANTIATE_TEST_SUITE_P(Sizes, RxTxAppFmdLoopbackTest,
                         ::testing::Values(1, 3, 4, 13, 1023, 1024, 1025, 1037, 2048,
                                           65536 + 3));

TEST_F(RxTxAppFmdTest, TxCutsThirteenByteFileIntoOneItemEachFrame) {
  const char text[] = "hello world!\n";
  std::string url = file(std::vector<uint8_t>(text, text + 13));
  ut_tx_st40p* tx = ut_tx_st40p_create(session_json(url).c_str());
  ASSERT_NE(tx, nullptr);
  ASSERT_EQ(ut_tx_st40p_run(tx, 4), 4);
  for (int i = 0; i < 4; i++) {
    uint8_t buf[64];
    EXPECT_EQ(ut_tx_st40p_fill(tx, i), 13u);
    ASSERT_EQ(ut_tx_st40p_item(tx, i, buf, sizeof(buf)), 16u);
    EXPECT_EQ(memcmp(buf, text, 13), 0);
  }
  ut_tx_st40p_free(tx);
}

TEST_F(RxTxAppFmdTest, TxWrapsAtEndOfFileOnChunkBoundary) {
  std::vector<uint8_t> payload = random_bytes(2 * 1024 + 5, 7);
  std::string url = file(payload);
  ut_tx_st40p* tx = ut_tx_st40p_create(session_json(url).c_str());
  ASSERT_NE(tx, nullptr);
  ASSERT_EQ(ut_tx_st40p_run(tx, 7), 7);
  const uint32_t expect[] = {1024, 1024, 5, 1024, 1024, 5, 1024};
  for (int i = 0; i < 7; i++) {
    uint8_t buf[2048];
    EXPECT_EQ(ut_tx_st40p_fill(tx, i), expect[i]) << "frame " << i;
    ut_tx_st40p_item(tx, i, buf, sizeof(buf));
    EXPECT_EQ(memcmp(buf, payload.data() + (size_t)(i % 3) * 1024, expect[i]), 0) << i;
  }
  ut_tx_st40p_free(tx);
}

/* a lost item: the lib flags seq_discont on the next one. RX resyncs, so no item is
 * invalid, but the loss fails the session */
TEST_F(RxTxAppFmdTest, SeqGapResyncsAndFails) {
  std::string url = file(random_bytes(65536 + 3, 11));
  std::vector<Item> all = tx_items(url, 300);
  ASSERT_EQ(all.size(), 300u);
  std::vector<Item> items;
  std::vector<bool> discont;
  for (size_t i = 0; i < all.size(); i++) {
    if (i >= 40 && i < 43) continue; /* lost */
    discont.push_back(i == 43);
    items.push_back(all[i]);
  }
  std::string line;
  EXPECT_EQ(rx_verdict(session_json(url), items, &line, discont), -EIO) << line;
  EXPECT_EQ(invalid_, 0) << line;
  EXPECT_NE(line.find("FAILED"), std::string::npos) << line;
}

/* the 13-byte file: every item is the same chunk, the resync after the gap finds it */
TEST_F(RxTxAppFmdTest, SeqGapWithThirteenByteFileResyncsAndFails) {
  const char text[] = "hello world!\n";
  std::string url = file(std::vector<uint8_t>(text, text + 13));
  std::vector<Item> all = tx_items(url, 120);
  std::vector<Item> items;
  std::vector<bool> discont;
  for (size_t i = 0; i < all.size(); i++) {
    if (i == 50) continue;
    discont.push_back(i == 51);
    items.push_back(all[i]);
  }
  std::string line;
  EXPECT_EQ(rx_verdict(session_json(url), items, &line, discont), -EIO) << line;
  EXPECT_EQ(invalid_, 0) << line;
}

/* chunks A A B: after a gap, an item that matches two chunks keeps RX unsynced, it
 * is not invalid */
TEST_F(RxTxAppFmdTest, RepeatingChunksAfterGapAreNotInvalid) {
  std::vector<uint8_t> a = random_bytes(1024, 21);
  std::vector<uint8_t> b = random_bytes(1024, 22);
  std::vector<uint8_t> payload = a;
  payload.insert(payload.end(), a.begin(), a.end());
  payload.insert(payload.end(), b.begin(), b.end());
  std::string url = file(payload);
  std::vector<Item> all = tx_items(url, 300);
  std::vector<Item> items;
  std::vector<bool> discont;
  for (size_t i = 0; i < all.size(); i++) {
    if (i == 100) continue;
    discont.push_back(i == 101);
    items.push_back(all[i]);
  }
  std::string line;
  EXPECT_EQ(rx_verdict(session_json(url), items, &line, discont), -EIO) << line;
  EXPECT_EQ(invalid_, 0) << line;
}

TEST_F(RxTxAppFmdTest, CorruptItemFails) {
  std::string url = file(random_bytes(65536 + 3, 31));
  std::vector<Item> items = tx_items(url, 300);
  items[77][5] ^= 0x01;
  std::string line;
  EXPECT_EQ(rx_verdict(session_json(url), items, &line), -EIO) << line;
  EXPECT_GE(invalid_, 1);
  EXPECT_NE(line.find("FAILED"), std::string::npos) << line;
}

TEST_F(RxTxAppFmdTest, NonZeroPaddingFails) {
  std::vector<uint8_t> payload = random_bytes(1024 + 13, 32);
  std::string url = file(payload);
  std::vector<Item> items = tx_items(url, 300);
  ASSERT_EQ(items[1].size(), 16u);
  items[1][14] = 0xff;
  EXPECT_EQ(rx_verdict(session_json(url), items), -EIO);
  EXPECT_GE(invalid_, 1);
}

/* an item repeated or swapped with an in-order rtp seq breaks the order */
TEST_F(RxTxAppFmdTest, RepeatedItemInSeqFails) {
  std::string url = file(random_bytes(65536 + 3, 33));
  std::vector<Item> items = tx_items(url, 300);
  items[60] = items[59];
  EXPECT_EQ(rx_verdict(session_json(url), items), -EIO);
}

TEST_F(RxTxAppFmdTest, SwappedItemsInSeqFail) {
  std::string url = file(random_bytes(65536 + 3, 34));
  std::vector<Item> items = tx_items(url, 300);
  std::swap(items[60], items[61]);
  EXPECT_EQ(rx_verdict(session_json(url), items), -EIO);
}

/* one item with a bad marker, DIT or K-bit is one invalid item: the items after it are
 * intact and in order, so the reference compare must still accept them */
TEST_F(RxTxAppFmdTest, OneBadHeaderItemCountsOneInvalid) {
  const struct {
    bool marker;
    uint32_t dit;
    uint8_t k_bit;
  } bad[] = {{true, kDit, 0}, {false, kDit + 1, 0}, {false, kDit, 1}};
  std::string url = file(random_bytes(4096, 35));
  std::vector<Item> items = tx_items(url, 120);
  for (auto& b : bad) {
    ut_rx_st40p* rx = ut_rx_st40p_create(session_json(url).c_str());
    ASSERT_NE(rx, nullptr);
    uint64_t t = 1000 * kNsPerMs;
    for (size_t i = 0; i < items.size(); i++, t += kFrameNs) {
      bool hit = i == 60;
      ut_rx_st40p_push(rx, items[i].data(), items[i].size(), hit ? b.dit : kDit,
                       hit ? b.k_bit : 0, hit && b.marker, false, 0, t);
    }
    ut_rx_st40p_run(rx);
    ut_rxtxapp_set_now(t);
    EXPECT_EQ(ut_rx_st40p_result(rx), -EIO);
    EXPECT_EQ(ut_rx_st40p_invalid(rx), 1)
        << "marker " << b.marker << " dit " << b.dit << " k_bit " << (int)b.k_bit;
    EXPECT_EQ(ut_rx_st40p_received(rx), 119);
    ut_rx_st40p_free(rx);
  }
}

/* a lost item at any place of the file cycle, alone or two in a row */
class RxTxAppFmdGapTest : public RxTxAppFmdTest,
                          public ::testing::WithParamInterface<size_t> {};

TEST_P(RxTxAppFmdGapTest, GapAtEveryPositionResyncsAndFails) {
  std::string url = file(random_bytes(GetParam(), 41));
  std::vector<Item> all = tx_items(url, 400);
  size_t cycle = (GetParam() + 1023) / 1024;
  for (size_t lost = 1; lost <= 2; lost++) {
    for (size_t at = 1; at < 2 * cycle + 2 && at + lost < all.size(); at++) {
      std::vector<Item> items;
      std::vector<bool> discont;
      for (size_t i = 0; i < all.size(); i++) {
        if (i >= at && i < at + lost) continue;
        discont.push_back(i == at + lost);
        items.push_back(all[i]);
      }
      std::string line;
      ASSERT_EQ(rx_verdict(session_json(url), items, &line, discont), -EIO)
          << "lost " << lost << " at " << at << ": " << line;
      ASSERT_EQ(invalid_, 0) << "lost " << lost << " at " << at << ": " << line;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Sizes, RxTxAppFmdGapTest,
                         ::testing::Values(13, 1024, 1037, 2048, 3 * 1024 + 1));

/* 0,2,1,3 on the wire: the lib flags 2, 1 and 3, RX resyncs on each */
TEST_F(RxTxAppFmdTest, NetworkReorderResyncsAndFails) {
  std::string url = file(random_bytes(65536 + 3, 42));
  std::vector<Item> items = tx_items(url, 300);
  std::swap(items[101], items[102]);
  std::vector<bool> discont(items.size(), false);
  discont[101] = discont[102] = discont[103] = true;
  std::string line;
  EXPECT_EQ(rx_verdict(session_json(url), items, &line, discont), -EIO) << line;
  EXPECT_EQ(invalid_, 0) << line;
}

/* every item after a seq gap: RX resyncs on each, so the reference compare never
 * checks the order of the file. The gaps must fail a stream that sends the file
 * backwards. */
TEST_F(RxTxAppFmdTest, SeqGapOnEveryItemStillChecksOrder) {
  std::string url = file(random_bytes(65536 + 3, 43));
  std::vector<Item> fwd = tx_items(url, 65);
  std::vector<Item> items;
  for (int rep = 0; rep < 5; rep++)
    for (int i = 64; i >= 0; i--) items.push_back(fwd[i]);
  std::vector<bool> discont(items.size(), true);
  std::string line;
  EXPECT_EQ(rx_verdict(session_json(url), items, &line, discont), -EIO) << line;
}

TEST_F(RxTxAppFmdTest, RxCreateFailUnmapsReference) {
  std::string url = file(random_bytes(4096, 44));
  auto maps_of = [&url]() {
    int n = 0;
    FILE* f = fopen("/proc/self/maps", "r");
    char line[4096];
    while (f && fgets(line, sizeof(line), f))
      if (strstr(line, url.c_str())) n++;
    if (f) (void)fclose(f);
    return n;
  };
  int fds = open_fds();
  ut_rx_st40p_fail_create(true);
  EXPECT_EQ(ut_rx_st40p_create(session_json(url).c_str()), nullptr);
  ut_rx_st40p_fail_create(false);
  EXPECT_EQ(maps_of(), 0);
  EXPECT_EQ(open_fds(), fds);
  ut_rx_st40p* rx = ut_rx_st40p_create(session_json(url).c_str());
  ASSERT_NE(rx, nullptr);
  EXPECT_EQ(maps_of(), 1);
  ut_rx_st40p_free(rx);
  EXPECT_EQ(maps_of(), 0);
}

/* §5.1: items 500 ms apart at most, the end of the run counts as well */
TEST_F(RxTxAppFmdTest, TailGapOverFiveHundredMsFails) {
  std::string url = file(random_bytes(4096, 36));
  std::vector<Item> items = tx_items(url, 600);
  std::string line;
  EXPECT_EQ(rx_verdict(session_json(url), items, &line, {}, 501 * kNsPerMs), -EIO)
      << line;
  EXPECT_EQ(rx_verdict(session_json(url), items, &line, {}, 499 * kNsPerMs), 0) << line;
}

TEST_F(RxTxAppFmdTest, RxWithoutReferenceChecksHeaderOnly) {
  std::string url = file(random_bytes(4096, 37));
  std::vector<Item> items = tx_items(url, 120);
  items[10][0] ^= 0xff;
  ut_rx_st40p* rx = ut_rx_st40p_create(session_json("").c_str());
  ASSERT_NE(rx, nullptr);
  EXPECT_FALSE(ut_rx_st40p_ref_mapped(rx));
  ut_rx_st40p_free(rx);
  EXPECT_EQ(rx_verdict(session_json(""), items), 0);
}

TEST_F(RxTxAppFmdTest, RxInitWithMissingReferenceFailsWithoutFdLeak) {
  int before = open_fds();
  std::string empty = file({});
  int with_file = open_fds();
  EXPECT_EQ(with_file, before);
  EXPECT_EQ(ut_rx_st40p_create(session_json("/nonexistent/fmd.bin").c_str()), nullptr);
  EXPECT_EQ(ut_rx_st40p_create(session_json(empty).c_str()), nullptr);
  EXPECT_EQ(ut_rx_st40p_create(session_json("/tmp").c_str()), nullptr);
  EXPECT_EQ(open_fds(), before);
}

/* the ancillary session gets the RTP packets and checks the RTP seq itself */
TEST_F(RxTxAppFmdTest, AncGoodStreamIsOk) {
  std::string url = file(random_bytes(65536 + 3, 45));
  std::vector<Item> items = tx_items(url, 300);
  std::vector<uint16_t> seqs;
  for (size_t i = 0; i < items.size(); i++) seqs.push_back((uint16_t)(65400 + i));
  EXPECT_EQ(anc_verdict(url, items, seqs), 0);
  EXPECT_EQ(invalid_, 0);
}

/* lost RTP packets: as with st40p, RX resyncs and the loss fails the session */
TEST_F(RxTxAppFmdTest, AncSeqGapResyncsAndFails) {
  std::string url = file(random_bytes(65536 + 3, 46));
  std::vector<Item> all = tx_items(url, 300);
  std::vector<Item> items;
  std::vector<uint16_t> seqs;
  for (size_t i = 0; i < all.size(); i++) {
    if (i >= 40 && i < 43) continue; /* lost */
    items.push_back(all[i]);
    seqs.push_back((uint16_t)i);
  }
  EXPECT_EQ(anc_verdict(url, items, seqs), -EIO);
  EXPECT_EQ(invalid_, 0);
}

/* json keys */
TEST_F(RxTxAppFmdTest, JsonTxNeedsBothItemFields) {
  struct ut_fmd_json out;
  ut_parse_st40p(
      "{\"start_port\": 30000, \"fps\": \"p59\", \"st40p_url\": \"x\", "
      "\"fast_metadata\": true, \"fastmetadata_k_bit\": 0}",
      true, &out);
  EXPECT_LT(out.ret, 0);
  ut_parse_st40p(
      "{\"start_port\": 30000, \"fps\": \"p59\", \"st40p_url\": \"x\", "
      "\"fast_metadata\": true, \"fastmetadata_data_item_type\": 5}",
      true, &out);
  EXPECT_LT(out.ret, 0);
}

TEST_F(RxTxAppFmdTest, JsonRxItemFieldsAreOptional) {
  struct ut_fmd_json out;
  ut_parse_st40p("{\"start_port\": 30000, \"fps\": \"p59\", \"fast_metadata\": true}",
                 false, &out);
  EXPECT_EQ(out.ret, 0);
  EXPECT_TRUE(out.fast_metadata);
  EXPECT_EQ(out.dit, -1);
  EXPECT_EQ(out.k_bit, -1);
  EXPECT_EQ(out.payload_type, 115);
  EXPECT_STREQ(out.url, "");
}

TEST_F(RxTxAppFmdTest, JsonRejectsBadItemFields) {
  const char* bad[] = {
      "\"fastmetadata_data_item_type\": 4194304, \"fastmetadata_k_bit\": 0",
      "\"fastmetadata_data_item_type\": -1, \"fastmetadata_k_bit\": 0",
      "\"fastmetadata_data_item_type\": 4294967297, \"fastmetadata_k_bit\": 0",
      "\"fastmetadata_data_item_type\": \"5\", \"fastmetadata_k_bit\": 0",
      "\"fastmetadata_data_item_type\": 5.0, \"fastmetadata_k_bit\": 0",
      "\"fastmetadata_data_item_type\": 5, \"fastmetadata_k_bit\": 2",
      "\"fastmetadata_data_item_type\": 5, \"fastmetadata_k_bit\": -1",
      "\"fastmetadata_data_item_type\": 5, \"fastmetadata_k_bit\": true",
  };
  for (const char* b : bad) {
    for (bool tx : {true, false}) {
      struct ut_fmd_json out;
      std::string j = std::string(
                          "{\"start_port\": 30000, \"fps\": \"p59\", \"st40p_url\": "
                          "\"x\", \"fast_metadata\": true, ") +
                      b + "}";
      ut_parse_st40p(j.c_str(), tx, &out);
      EXPECT_LT(out.ret, 0) << (tx ? "tx " : "rx ") << b;
      ut_parse_anc(j.c_str(), tx, &out);
      EXPECT_LT(out.ret, 0) << (tx ? "anc tx " : "anc rx ") << b;
    }
  }
}

TEST_F(RxTxAppFmdTest, JsonAcceptsItemFieldEdges) {
  const struct {
    int dit, k_bit;
  } good[] = {{0, 0}, {0x3fffff, 1}};
  for (auto& g : good) {
    struct ut_fmd_json out;
    std::string j =
        "{\"start_port\": 30000, \"fps\": \"p59\", \"st40p_url\": \"x\", "
        "\"fast_metadata\": true, \"fastmetadata_data_item_type\": " +
        std::to_string(g.dit) + ", \"fastmetadata_k_bit\": " + std::to_string(g.k_bit) +
        "}";
    ut_parse_st40p(j.c_str(), true, &out);
    EXPECT_EQ(out.ret, 0);
    EXPECT_EQ(out.dit, g.dit);
    EXPECT_EQ(out.k_bit, g.k_bit);
    EXPECT_EQ(out.payload_type, 115);
  }
}

/* json_object_get_boolean reads any non-empty string as true, "false" too: so
 * fast_metadata must be a json boolean, any other type is a parse error */
TEST_F(RxTxAppFmdTest, JsonFastMetadataStringFalseIsNotTrue) {
  auto json = [](const char* value) {
    return std::string(
               "{\"start_port\": 30000, \"fps\": \"p59\", \"st40p_url\": \"x\", "
               "\"fastmetadata_data_item_type\": 5, \"fastmetadata_k_bit\": 0, "
               "\"fast_metadata\": ") +
           value + "}";
  };
  for (bool tx : {true, false}) {
    for (const char* v : {"\"false\"", "\"true\"", "0", "1"}) {
      struct ut_fmd_json out;
      ut_parse_st40p(json(v).c_str(), tx, &out);
      EXPECT_LT(out.ret, 0) << (tx ? "tx " : "rx ") << v;
      EXPECT_FALSE(out.fast_metadata) << (tx ? "tx " : "rx ") << v;
      ut_parse_anc(json(v).c_str(), tx, &out);
      EXPECT_LT(out.ret, 0) << (tx ? "anc tx " : "anc rx ") << v;
      EXPECT_FALSE(out.fast_metadata) << (tx ? "anc tx " : "anc rx ") << v;
    }
    struct ut_fmd_json out;
    ut_parse_st40p(json("false").c_str(), tx, &out);
    EXPECT_EQ(out.ret, 0) << (tx ? "tx" : "rx");
    EXPECT_FALSE(out.fast_metadata) << (tx ? "tx" : "rx");
  }
}

TEST_F(RxTxAppFmdTest, TxInitSetsFastMetadataOps) {
  std::string url = file(random_bytes(100, 39));
  ut_tx_st40p* tx = ut_tx_st40p_create(session_json(url).c_str());
  ASSERT_NE(tx, nullptr);
  EXPECT_TRUE(ut_tx_st40p_ops_flags(tx) & ST40P_TX_FLAG_FAST_METADATA);
  ut_tx_st40p_free(tx);
  ut_rx_st40p* rx = ut_rx_st40p_create(session_json(url).c_str());
  ASSERT_NE(rx, nullptr);
  EXPECT_TRUE(ut_rx_st40p_ops_flags(rx) & ST40P_RX_FLAG_FAST_METADATA);
  ut_rx_st40p_free(rx);
}
