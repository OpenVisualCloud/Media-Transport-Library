/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * The ts-refclk string of the IPMX Info Block of an st20 TX session (task 9).
 *
 * TR-10-1 section 10.4 asks for the ST 2110-10 ptp form while a Common Reference Clock is
 * present and localmac otherwise; TR-10-9 section 12 asks the sender to update it when
 * the sync status changes; TR-10-1 section 8.7 bumps the block version whenever the Info
 * Block content changes. The library does this at each packet 0 of the primary port:
 * ptp=IEEE1588-2008:<GM EUI-64>:<domain> while the built-in PTP has a master, a delay
 * exchange, a domain of 0..127 and an Announce younger than 3 announce intervals, else
 * localmac=<P MAC>. An application with its own PTP time overrides the string with
 * ipmx_st20[p]_tx_set_ts_refclk().
 *
 * Groups:
 * - IpmxRefclkDecisionTest, IpmxRefclkIntervalTest: the pure decision
 *   ipmx_refclk_from_ptp() over a PTP state snapshot and the current time.
 * - IpmxRefclkPrintTest: the string ipmx_refclk_print() writes, golden values.
 * - IpmxRefclkSessionTest: a session on the SR harness (ipmx_sr_harness.h) with a fake
 *   PTP instance. Each NextInfoBlock() runs one frame's packet 0 through the transmitter
 *   path and returns the Info Block of the SR built for it, so a case checks what goes on
 *   the wire frame by frame: the string, its zero padding and the version byte. Info()
 *   is what ipmx_st20_tx_get_info() returns. The cases cover the PTP sources that must
 *   stay localmac, the version bump rules, getter consistency, the app override and the
 *   seqcount retry paths (a packet 0 during an Announce or setter write).
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*IpmxRefclk*'
 * (the leading * also matches the parameterised Intervals/IpmxRefclkIntervalTest)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ipmx/ipmx_info.h"
#include "ipmx/ipmx_sr_harness.h"

namespace {

/* SR harness session: source IP, primary port, RTP timing of the frames it sends */
constexpr uint8_t kSip[4] = {192, 168, 1, 10};
constexpr uint64_t kTaiNs = 1700000000ull * 1000000000ull;
constexpr uint32_t kRtpTs = 1000;
constexpr int kPortP = 0;
/* 1 s in ns */
constexpr uint64_t kNs = 1000000000ull;
/* the harness clock (mt_get_tsc()) used as "now" unless a case moves it */
constexpr uint64_t kNow = 100 * kNs;
/* the TR-10-1 section 8.7 example grandmaster */
constexpr uint64_t kGm = 0xEC4670FFFE10FFB0ull;
/* the Info Block follows the 28-byte SR header; byte 4 of it is the block version */
constexpr size_t kInfoBlockOffset = 28;
constexpr size_t kVersion = IPMX_INFO_BLOCK_VERSION_OFFSET;
/* the localmac form of the harness port MAC */
const char* const kLocalmac = "localmac=02-00-00-00-00-01";
/* the ptp form of kGm at domain 127 */
const char* const kGmRefclk = "ptp=IEEE1588-2008:EC-46-70-FF-FE-10-FF-B0:127";

/* a snapshot that gives the ptp form: built-in PTP, delay exchange done, Announce 1 s
 * old at a 1 s interval (timeout 3 s), the example grandmaster, domain 127 */
struct ipmx_ptp_snap GoodSnap() {
  struct ipmx_ptp_snap snap;
  memset(&snap, 0, sizeof(snap));
  snap.builtin = true;
  snap.connected = true;
  snap.announce_ns = kNow - kNs;
  snap.log_announce_interval = 0;
  snap.gm_id = kGm;
  snap.domain = 127;
  return snap;
}

/* ---- ipmx_refclk_from_ptp(): when the ptp form applies ---- */

/* All conditions met: the ptp form with the snapshot's grandmaster and domain. */
TEST(IpmxRefclkDecisionTest, FreshMasterGivesItsGrandmasterAndDomain) {
  struct ipmx_ptp_snap snap = GoodSnap();
  struct ipmx_refclk clk = ipmx_refclk_from_ptp(&snap, kNow);
  EXPECT_TRUE(clk.ptp);
  EXPECT_EQ(clk.gm_id, kGm);
  EXPECT_EQ(clk.domain, 127);
}

/* Time not from the built-in PTP (no PTP, app time fn, TSC source): localmac. */
TEST(IpmxRefclkDecisionTest, NoBuiltinPtpIsLocalmac) {
  struct ipmx_ptp_snap snap = GoodSnap();
  snap.builtin = false;
  EXPECT_FALSE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
}

/* No Announce from a master yet (arrival time 0): localmac. */
TEST(IpmxRefclkDecisionTest, NoMasterIsLocalmac) {
  struct ipmx_ptp_snap snap = GoodSnap();
  snap.announce_ns = 0;
  EXPECT_FALSE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
}

/* A master is selected but no delay request/response exchange finished: localmac. */
TEST(IpmxRefclkDecisionTest, MasterWithoutDelayExchangeIsLocalmac) {
  struct ipmx_ptp_snap snap = GoodSnap();
  snap.connected = false;
  EXPECT_FALSE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
}

/* The localmac result has no grandmaster or domain left over, so comparing two results
 * for a change compares only meaningful fields. */
TEST(IpmxRefclkDecisionTest, LocalmacCarriesNoGrandmaster) {
  struct ipmx_ptp_snap snap = GoodSnap();
  snap.connected = false;
  struct ipmx_refclk clk = ipmx_refclk_from_ptp(&snap, kNow);
  EXPECT_EQ(clk.gm_id, 0u);
  EXPECT_EQ(clk.domain, 0);
}

/* The IEEE 1588 announceReceiptTimeout of 3 intervals: exactly 3 s old at a 1 s interval
 * is stale, 1 ns less is still fresh. */
TEST(IpmxRefclkDecisionTest, AnnounceStaleAtThreeIntervals) {
  struct ipmx_ptp_snap snap = GoodSnap();
  snap.announce_ns = kNow - 3 * kNs;
  EXPECT_FALSE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
  snap.announce_ns = kNow - 3 * kNs + 1;
  EXPECT_TRUE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
}

/* the timeout for each log announce interval, including the clamp to -3..4 */
struct IntervalCase {
  const char* name;
  int8_t log_interval;
  uint64_t timeout_ns;
};

class IpmxRefclkIntervalTest : public ::testing::TestWithParam<IntervalCase> {};

/* 1 ns before the timeout is fresh, at the timeout is stale. */
TEST_P(IpmxRefclkIntervalTest, TimeoutIsThreeAnnounceIntervals) {
  const IntervalCase& c = GetParam();
  struct ipmx_ptp_snap snap = GoodSnap();
  snap.log_announce_interval = c.log_interval;
  snap.announce_ns = kNow - c.timeout_ns + 1;
  EXPECT_TRUE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
  snap.announce_ns = kNow - c.timeout_ns;
  EXPECT_FALSE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
}

INSTANTIATE_TEST_SUITE_P(
    Intervals, IpmxRefclkIntervalTest,
    ::testing::Values(IntervalCase{"Log0", 0, 3 * kNs}, IntervalCase{"Log1", 1, 6 * kNs},
                      IntervalCase{"Log4", 4, 48 * kNs},
                      IntervalCase{"LogMinus3", -3, 375000000ull},
                      IntervalCase{"ClampedHigh", 127, 48 * kNs},
                      IntervalCase{"ClampedLow", -128, 375000000ull}),
    [](const ::testing::TestParamInfo<IntervalCase>& info) {
      return std::string(info.param.name);
    });

/* RFC 7273 allows an IEEE1588-2008 domain of 0..127 only; a master announcing a higher
 * one gives no valid ptp form, so localmac. */
TEST(IpmxRefclkDecisionTest, DomainAbove127IsLocalmac) {
  struct ipmx_ptp_snap snap = GoodSnap();
  snap.domain = 128;
  EXPECT_FALSE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
  snap.domain = 255;
  EXPECT_FALSE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
}

/* The PTP thread can store an Announce after the transmitter read "now"; the signed age
 * then is negative and must count as fresh, not as a huge unsigned age. */
TEST(IpmxRefclkDecisionTest, AnnounceStampedAfterNowIsFresh) {
  struct ipmx_ptp_snap snap = GoodSnap();
  snap.announce_ns = kNow + 1000;
  EXPECT_TRUE(ipmx_refclk_from_ptp(&snap, kNow).ptp);
}

/* ---- ipmx_refclk_print(): the string ---- */

/* The ST 2110-10 section 8.2 form, upper-case EUI-64, zero padded to the 64-byte field
 * (the 0xee fill shows any byte not written). */
TEST(IpmxRefclkPrintTest, PtpFormGolden) {
  struct ipmx_refclk clk = {true, 127, kGm};
  char out[IPMX_TS_REFCLK_LEN];
  memset(out, 0xee, sizeof(out));
  ipmx_refclk_print(&clk, kLocalmac, out);
  EXPECT_STREQ(out, kGmRefclk);
  for (size_t i = strlen(kGmRefclk); i < sizeof(out); i++) EXPECT_EQ(out[i], 0) << i;
}

/* Each octet keeps its leading zero and domain 0 prints as "0". */
TEST(IpmxRefclkPrintTest, PtpFormDomainZeroAndLeadingZeros) {
  struct ipmx_refclk clk = {true, 0, 0x000102030405060Aull};
  char out[IPMX_TS_REFCLK_LEN];
  ipmx_refclk_print(&clk, kLocalmac, out);
  EXPECT_STREQ(out, "ptp=IEEE1588-2008:00-01-02-03-04-05-06-0A:0");
}

/* The localmac result is the prepared, zero-padded localmac field, all 64 bytes copied.
 */
TEST(IpmxRefclkPrintTest, LocalmacFormCopiedZeroPadded) {
  struct ipmx_refclk clk = {false, 0, 0};
  char localmac[IPMX_TS_REFCLK_LEN] = {0};
  char out[IPMX_TS_REFCLK_LEN];
  snprintf(localmac, sizeof(localmac), "%s", kLocalmac);
  memset(out, 0xee, sizeof(out));
  ipmx_refclk_print(&clk, localmac, out);
  EXPECT_EQ(0, memcmp(out, localmac, sizeof(out)));
}

/* ---- the session: what each frame's SR carries ---- */

/* An IPMX st20 TX session on the SR harness. Cases set the fake PTP state with
 * ut_sr_set_ptp() before or after ut_sr_attach() (session create), then step frames with
 * NextInfoBlock(). */
class IpmxRefclkSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
    ctx_ = ut_sr_create(kSip, 1, 20000);
    ASSERT_NE(ctx_, nullptr);
    ut_sr_set_now(ctx_, kNow);
  }
  void TearDown() override {
    ut_sr_destroy(ctx_);
  }
  /* PTP state that gives the ptp form, the session counterpart of GoodSnap() */
  static struct ut_sr_ptp GoodPtp() {
    struct ut_sr_ptp ptp;
    memset(&ptp, 0, sizeof(ptp));
    ptp.ptp_service = true;
    ptp.active = true;
    ptp.connected = true;
    ptp.announce_ns = kNow - kNs;
    ptp.gm_id = kGm;
    ptp.domain = 127;
    return ptp;
  }
  /* the Info Block of the SR of the next frame */
  std::vector<uint8_t> NextInfoBlock() {
    ut_sr_frame(ctx_, kPortP, kTaiNs + frames_ * 16683333ull, kRtpTs + frames_ * 1501,
                false);
    frames_++;
    uint8_t pkt[UT_SR_PKT_MAX];
    uint16_t len = ut_sr_last_sr(ctx_, pkt);
    const uint8_t* block = pkt + UT_SR_UDP_HDR_LEN + kInfoBlockOffset;
    EXPECT_GE(len, UT_SR_UDP_HDR_LEN + kInfoBlockOffset + IPMX_INFO_BLOCK_LEN);
    return std::vector<uint8_t>(block, block + IPMX_INFO_BLOCK_LEN);
  }
  /* the ts-refclk field of an Info Block, without its padding */
  static std::string Refclk(const std::vector<uint8_t>& block) {
    const char* s = (const char*)&block[IPMX_TS_REFCLK_OFFSET];
    return std::string(s, strnlen(s, IPMX_TS_REFCLK_LEN));
  }
  /* every byte after the string in the 64-byte field is 0 (TR-10-1 section 8.7) */
  static bool RefclkZeroPadded(const std::vector<uint8_t>& block) {
    const uint8_t* s = &block[IPMX_TS_REFCLK_OFFSET];
    size_t n = strnlen((const char*)s, IPMX_TS_REFCLK_LEN);
    for (size_t i = n; i < IPMX_TS_REFCLK_LEN; i++)
      if (s[i]) return false;
    return true;
  }
  /* what ipmx_st20_tx_get_info() returns now */
  struct ipmx_video_info Info() {
    struct ipmx_video_info info;
    EXPECT_EQ(ut_sr_get_info(ctx_, &info), 0);
    return info;
  }
  ut_sr_ctx* ctx_ = nullptr;
  int frames_ = 0;
};

/* No PTP instance on the port at all: localmac, version 0. */
TEST_F(IpmxRefclkSessionTest, NoPtpInstanceIsLocalmac) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kLocalmac);
  EXPECT_EQ(block[kVersion], 0);
}

/* PTP already up at create: the getter and the first SR carry the ptp form at version 0,
 * since nothing changed after create. */
TEST_F(IpmxRefclkSessionTest, PtpAtCreateIsPtpFormAtVersionZero) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  struct ipmx_video_info info = Info();
  EXPECT_STREQ(info.ts_refclk, kGmRefclk);
  EXPECT_EQ(info.version, 0);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kGmRefclk);
  EXPECT_TRUE(RefclkZeroPadded(block));
  EXPECT_EQ(block[kVersion], 0);
}

/* PTP_ENABLE off: ptp_init() marks the port active, connected and locked with no master.
 * "locked" alone must not give the ptp form; it stays localmac with no bump. */
TEST_F(IpmxRefclkSessionTest, LockedWithoutPtpServiceIsLocalmac) {
  /* ptp_init() without MTL_FLAG_PTP_ENABLE: active, connected and locked, no master */
  struct ut_sr_ptp ptp = GoodPtp();
  ptp.ptp_service = false;
  ptp.announce_ns = 0;
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  for (int i = 0; i < 3; i++) {
    std::vector<uint8_t> block = NextInfoBlock();
    EXPECT_EQ(Refclk(block), kLocalmac);
    EXPECT_EQ(block[kVersion], 0);
  }
}

/* A PTP instance that is not running: localmac. */
TEST_F(IpmxRefclkSessionTest, InactivePtpIsLocalmac) {
  struct ut_sr_ptp ptp = GoodPtp();
  ptp.active = false;
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Refclk(NextInfoBlock()), kLocalmac);
}

/* The app supplies PTP time (ptp_get_time_fn): the library cannot know its master, so
 * localmac unless the app sets the string (see the AppSet cases). */
TEST_F(IpmxRefclkSessionTest, UserPtpTimeFnIsLocalmac) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ut_sr_set_ptp_source(ctx_, false, true);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Refclk(NextInfoBlock()), kLocalmac);
}

/* MTL_FLAG_PTP_SOURCE_TSC: media time is the TSC, not PTP, so localmac. */
TEST_F(IpmxRefclkSessionTest, PtpSourceTscIsLocalmac) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ut_sr_set_ptp_source(ctx_, true, false);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Refclk(NextInfoBlock()), kLocalmac);
}

/* ---- the block version ---- */

/* PTP connects after the first SR: the next SR carries the ptp form and version 1
 * together, and no other Info Block byte changes. The getter reports the same. */
TEST_F(IpmxRefclkSessionTest, PtpAfterCreateBumpsTheVersionInTheSameSr) {
  struct ut_sr_ptp ptp = GoodPtp();
  ptp.connected = false;
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  std::vector<uint8_t> before = NextInfoBlock();
  EXPECT_EQ(Refclk(before), kLocalmac);
  EXPECT_EQ(before[kVersion], 0);

  ptp.connected = true;
  ut_sr_set_ptp(ctx_, &ptp);
  std::vector<uint8_t> after = NextInfoBlock();
  EXPECT_EQ(Refclk(after), kGmRefclk);
  EXPECT_TRUE(RefclkZeroPadded(after));
  EXPECT_EQ(after[kVersion], 1);

  /* nothing else in the block changed */
  memcpy(&after[IPMX_TS_REFCLK_OFFSET], &before[IPMX_TS_REFCLK_OFFSET],
         IPMX_TS_REFCLK_LEN);
  after[kVersion] = before[kVersion];
  EXPECT_EQ(after, before);

  struct ipmx_video_info info = Info();
  EXPECT_STREQ(info.ts_refclk, kGmRefclk);
  EXPECT_EQ(info.version, 1);
}

/* A change before any SR went out is not a change a receiver saw: no bump, the first SR
 * already has the new string at version 0. */
TEST_F(IpmxRefclkSessionTest, PtpBetweenCreateAndTheFirstSrIsNoBump) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_STREQ(Info().ts_refclk, kLocalmac);
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kGmRefclk);
  EXPECT_EQ(block[kVersion], 0);
}

/* A steady master over several frames keeps the string and version 0. */
TEST_F(IpmxRefclkSessionTest, NoChangeNoBump) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  for (int i = 0; i < 5; i++) {
    std::vector<uint8_t> block = NextInfoBlock();
    EXPECT_EQ(Refclk(block), kGmRefclk);
    EXPECT_EQ(block[kVersion], 0);
  }
}

/* A new grandmaster behind the same master port: new string, version + 1. */
TEST_F(IpmxRefclkSessionTest, GrandmasterChangeBumps) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  NextInfoBlock();
  ptp.gm_id = 0x0011223344556677ull;
  ut_sr_set_ptp(ctx_, &ptp);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), "ptp=IEEE1588-2008:00-11-22-33-44-55-66-77:127");
  EXPECT_EQ(block[kVersion], 1);
}

/* A new domain from the same master: new string, version + 1. */
TEST_F(IpmxRefclkSessionTest, DomainChangeBumps) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  NextInfoBlock();
  ptp.domain = 0;
  ut_sr_set_ptp(ctx_, &ptp);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), "ptp=IEEE1588-2008:EC-46-70-FF-FE-10-FF-B0:0");
  EXPECT_EQ(block[kVersion], 1);
}

/* The master stops announcing: after 3 intervals localmac (version 1); a fresh Announce
 * brings the ptp form back (version 2). Each transition is one bump. */
TEST_F(IpmxRefclkSessionTest, StaleAnnounceFallsBackToLocalmacAndBack) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 0);

  ut_sr_set_now(ctx_, ptp.announce_ns + 3 * kNs);
  std::vector<uint8_t> stale = NextInfoBlock();
  EXPECT_EQ(Refclk(stale), kLocalmac);
  EXPECT_TRUE(RefclkZeroPadded(stale));
  EXPECT_EQ(stale[kVersion], 1);

  ptp.announce_ns = ptp.announce_ns + 3 * kNs;
  ut_sr_set_ptp(ctx_, &ptp);
  std::vector<uint8_t> fresh = NextInfoBlock();
  EXPECT_EQ(Refclk(fresh), kGmRefclk);
  EXPECT_EQ(fresh[kVersion], 2);
}

/* The library bump adds 1 to whatever version the app set (D9: the app owns the counter).
 */
TEST_F(IpmxRefclkSessionTest, BumpAddsOneToTheAppVersion) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 7), 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 7);
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  EXPECT_EQ(NextInfoBlock()[kVersion], 8);
  EXPECT_EQ(Info().version, 8);
}

/* The 8-bit version wraps from 255 to 0. */
TEST_F(IpmxRefclkSessionTest, BumpWrapsFrom255ToZero) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 255), 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 255);
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kGmRefclk);
  EXPECT_EQ(block[kVersion], 0);
}

/* ---- ipmx_st20_tx_get_info() consistency ---- */

/* After every SR, alternating localmac and ptp form, the getter returns the exact string
 * bytes and version of that SR, so an app republishing its SDP from it matches the wire.
 */
TEST_F(IpmxRefclkSessionTest, GetterMatchesTheSrAfterEachChange) {
  struct ut_sr_ptp ptp = GoodPtp();
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  for (int i = 0; i < 4; i++) {
    ptp.connected = i % 2;
    ut_sr_set_ptp(ctx_, &ptp);
    std::vector<uint8_t> block = NextInfoBlock();
    struct ipmx_video_info info = Info();
    EXPECT_EQ(Refclk(block), std::string(info.ts_refclk));
    EXPECT_EQ(block[kVersion], info.version);
    EXPECT_EQ(0,
              memcmp(&block[IPMX_TS_REFCLK_OFFSET], info.ts_refclk, IPMX_TS_REFCLK_LEN));
  }
}

/* A reader thread calls the getter while the transmitter changes the string 250 times.
 * Each change sets the grandmaster's last octet to the version it bumps to, so a read
 * that mixes the string of one state with the version of another is detected as torn. */
TEST_F(IpmxRefclkSessionTest, GetterNeverMixesTwoStates) {
  constexpr int kChanges = 250;
  struct ut_sr_ptp ptp = GoodPtp();
  ptp.gm_id = 0;
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  NextInfoBlock();

  std::atomic<bool> done{false};
  std::atomic<int> torn{0};
  std::atomic<int> reads{0};
  std::thread reader([&]() {
    while (!done.load()) {
      struct ipmx_video_info info;
      if (ut_sr_get_info(ctx_, &info) < 0) {
        torn++;
        continue;
      }
      char expect[IPMX_TS_REFCLK_LEN];
      snprintf(expect, sizeof(expect), "ptp=IEEE1588-2008:00-00-00-00-00-00-00-%02X:127",
               info.version);
      if (strncmp(info.ts_refclk, expect, sizeof(expect))) torn++;
      reads++;
    }
  });
  for (int i = 1; i <= kChanges; i++) {
    int seen = reads.load();
    ptp.gm_id = i;
    ut_sr_set_ptp(ctx_, &ptp);
    NextInfoBlock();
    while (reads.load() == seen) std::this_thread::yield();
  }
  done = true;
  reader.join();
  EXPECT_GT(reads.load(), 0);
  EXPECT_EQ(torn.load(), 0);
  EXPECT_EQ(Info().version, kChanges);
}

/* The PTP thread is mid-way through storing an Announce (seqcount odd) at packet 0: the
 * transmitter keeps the old string and version rather than read a mix of two Announces,
 * and takes the new state at the next frame. */
TEST_F(IpmxRefclkSessionTest, Packet0DuringAnAnnounceWriteKeepsTheOldString) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Refclk(NextInfoBlock()), kGmRefclk);

  ut_sr_ptp_announce_begin(ctx_, 0x0011223344556677ull, 2, kNow - kNs + 1);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kGmRefclk);
  EXPECT_EQ(block[kVersion], 0);
  ut_sr_ptp_announce_end(ctx_);
  block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), "ptp=IEEE1588-2008:00-11-22-33-44-55-66-77:2");
  EXPECT_EQ(block[kVersion], 1);
}

/* ---- ipmx_st20_tx_set_ts_refclk(): the app override ---- */

/* the string an app builds from pmc PARENT_DATA_SET / TIME_STATUS_NP */
const char* const kAppRefclk = "ptp=IEEE1588-2008:00-11-22-33-44-55-66-77:5";

/* Set right after create: the first SR and the getter carry it at version 0. */
TEST_F(IpmxRefclkSessionTest, AppSetBeforeTheFirstSrIsNoBump) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, kAppRefclk), 0);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kAppRefclk);
  EXPECT_TRUE(RefclkZeroPadded(block));
  EXPECT_EQ(block[kVersion], 0);
  struct ipmx_video_info info = Info();
  EXPECT_STREQ(info.ts_refclk, kAppRefclk);
  EXPECT_EQ(info.version, 0);
}

/* Set while streaming: the next SR carries it with version + 1. */
TEST_F(IpmxRefclkSessionTest, AppSetChangeBumpsInTheNextSr) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Refclk(NextInfoBlock()), kLocalmac);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, kAppRefclk), 0);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kAppRefclk);
  EXPECT_EQ(block[kVersion], 1);
  struct ipmx_video_info info = Info();
  EXPECT_STREQ(info.ts_refclk, kAppRefclk);
  EXPECT_EQ(info.version, 1);
}

/* The app may also set a localmac form, e.g. when pmc reports gmPresent false. */
TEST_F(IpmxRefclkSessionTest, AppSetLocalmacAccepted) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  NextInfoBlock();
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, "localmac=00-20-FC-32-2F-40"), 0);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), "localmac=00-20-FC-32-2F-40");
  EXPECT_EQ(block[kVersion], 1);
}

/* Setting the string already on the wire, or the same override again, is no change. */
TEST_F(IpmxRefclkSessionTest, AppSetOfTheSameStringIsNoBump) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  NextInfoBlock();
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, kLocalmac), 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 0);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, kAppRefclk), 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 1);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, kAppRefclk), 0);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kAppRefclk);
  EXPECT_EQ(block[kVersion], 1);
}

/* While the override is set, built-in PTP changes (domain) and a stale Announce leave the
 * app string and version alone. */
TEST_F(IpmxRefclkSessionTest, AppSetStopsTheAutomaticTracking) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Refclk(NextInfoBlock()), kGmRefclk);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, kAppRefclk), 0);
  EXPECT_EQ(NextInfoBlock()[kVersion], 1);

  ptp.domain = 1;
  ut_sr_set_ptp(ctx_, &ptp);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kAppRefclk);
  EXPECT_EQ(block[kVersion], 1);
  ut_sr_set_now(ctx_, kNow + 60 * kNs);
  block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kAppRefclk);
  EXPECT_EQ(block[kVersion], 1);
}

/* NULL drops the override: the next SR carries the current automatic string (here the
 * domain changed meanwhile), with a bump. */
TEST_F(IpmxRefclkSessionTest, AppSetNullRestoresTheAutomaticString) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  NextInfoBlock();
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, kAppRefclk), 0);
  NextInfoBlock();
  ptp.domain = 1;
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, nullptr), 0);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), "ptp=IEEE1588-2008:EC-46-70-FF-FE-10-FF-B0:1");
  EXPECT_EQ(block[kVersion], 2);
  EXPECT_STREQ(Info().ts_refclk, "ptp=IEEE1588-2008:EC-46-70-FF-FE-10-FF-B0:1");
}

/* Dropping an override equal to the automatic string changes nothing on the wire. */
TEST_F(IpmxRefclkSessionTest, AppSetNullBackToTheSameStringIsNoBump) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, kLocalmac), 0);
  NextInfoBlock();
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, nullptr), 0);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kLocalmac);
  EXPECT_EQ(block[kVersion], 0);
}

/* NULL is taken at a packet 0 where the PTP snapshot fails (Announce write in progress):
 * the old string stays, and the next frame still re-evaluates the automatic string
 * instead of assuming it is already on the wire. */
TEST_F(IpmxRefclkSessionTest, AppSetNullDuringAnAnnounceWriteRetries) {
  struct ut_sr_ptp ptp = GoodPtp();
  ut_sr_set_ptp(ctx_, &ptp);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, kAppRefclk), 0);
  EXPECT_EQ(Refclk(NextInfoBlock()), kAppRefclk);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, nullptr), 0);
  ut_sr_ptp_announce_begin(ctx_, kGm, 127, kNow - kNs + 1);
  EXPECT_EQ(Refclk(NextInfoBlock()), kAppRefclk);
  ut_sr_ptp_announce_end(ctx_);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kGmRefclk);
  EXPECT_EQ(block[kVersion], 1);
}

/* 63 characters plus the terminating zero fill the 64-byte field exactly. */
TEST_F(IpmxRefclkSessionTest, AppSetOf63CharsAccepted) {
  std::string longest = "ptp=" + std::string(IPMX_TS_REFCLK_LEN - 1 - 4, 'A');
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, longest.c_str()), 0);
  EXPECT_EQ(Refclk(NextInfoBlock()), longest);
}

/* -EINVAL for: 64+ chars, empty, other sources or case, a bare prefix, control, space,
 * DEL and non-ASCII bytes. None of them reaches the wire. */
TEST_F(IpmxRefclkSessionTest, AppSetInvalidRejectedAndWireUnchanged) {
  const std::string too_long = "ptp=" + std::string(IPMX_TS_REFCLK_LEN - 4, 'A');
  const char* const bad[] = {
      too_long.c_str(),
      "",
      "ntp=1",
      "PTP=IEEE1588-2008:00-00-00-00-00-00-00-00:0",
      "localmac",
      "ptp=IEEE1588-2008:\x01",
      "localmac=00-20-FC-32-2F-40 x",
      "ptp=\x7f",
      "ptp=\xc3\xa9",
      "ptp=",
      "localmac=",
  };
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  NextInfoBlock();
  for (const char* b : bad) EXPECT_EQ(ut_sr_set_ts_refclk(ctx_, b), -EINVAL) << b;
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kLocalmac);
  EXPECT_EQ(block[kVersion], 0);
}

/* A session that was never attached with IPMX has no SR state: -EIO, also for NULL. */
TEST_F(IpmxRefclkSessionTest, AppSetIpmxOffSessionRejected) {
  EXPECT_EQ(ut_sr_set_ts_refclk(ctx_, kAppRefclk), -EIO);
  EXPECT_EQ(ut_sr_set_ts_refclk(ctx_, nullptr), -EIO);
}

/* The setter is mid-way through writing (seqcount odd) at packet 0: the transmitter keeps
 * the old string without waiting, and takes the new one at the next frame. */
TEST_F(IpmxRefclkSessionTest, Packet0DuringAnAppSetKeepsTheOldString) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  NextInfoBlock();
  ut_sr_set_ts_refclk_begin(ctx_, kAppRefclk);
  std::vector<uint8_t> block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kLocalmac);
  EXPECT_EQ(block[kVersion], 0);
  ut_sr_set_ts_refclk_end(ctx_);
  block = NextInfoBlock();
  EXPECT_EQ(Refclk(block), kAppRefclk);
  EXPECT_EQ(block[kVersion], 1);
}

}  // namespace
