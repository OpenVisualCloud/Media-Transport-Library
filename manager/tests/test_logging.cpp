/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/* The log level, which --log-level sets from a name. */

#include <gtest/gtest.h>

#include <string>

#include "logging.hpp"

namespace {

/* Put the level back, so the order of the cases does not matter. */
class level_guard {
 public:
  level_guard() : old(logger::get_log_level()) {
  }
  ~level_guard() {
    logger::set_log_level(old);
  }

 private:
  log_level old;
};

} /* namespace */

TEST(Logging, EveryNameIsAccepted) {
  level_guard guard;

  EXPECT_TRUE(logger::set_log_level(std::string("debug")));
  EXPECT_EQ(logger::get_log_level(), log_level::DEBUG);

  EXPECT_TRUE(logger::set_log_level(std::string("info")));
  EXPECT_EQ(logger::get_log_level(), log_level::INFO);

  EXPECT_TRUE(logger::set_log_level(std::string("warning")));
  EXPECT_EQ(logger::get_log_level(), log_level::WARNING);

  EXPECT_TRUE(logger::set_log_level(std::string("error")));
  EXPECT_EQ(logger::get_log_level(), log_level::ERROR);
}

TEST(Logging, AnUnknownNameChangesNothing) {
  level_guard guard;

  logger::set_log_level(log_level::WARNING);
  EXPECT_FALSE(logger::set_log_level(std::string("verbose")));
  EXPECT_FALSE(logger::set_log_level(std::string("")));
  EXPECT_EQ(logger::get_log_level(), log_level::WARNING);
}

TEST(Logging, EveryLevelHasAName) {
  EXPECT_STREQ(logger::level_string(log_level::DEBUG), "DEBUG");
  EXPECT_STREQ(logger::level_string(log_level::INFO), "INFO");
  EXPECT_STREQ(logger::level_string(log_level::WARNING), "WARNING");
  EXPECT_STREQ(logger::level_string(log_level::ERROR), "ERROR");
}

TEST(Logging, ALogBelowTheLevelIsQuiet) {
  level_guard guard;

  logger::set_log_level(log_level::ERROR);

  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  logger::log(log_level::INFO, "this line must not appear");
  logger::log(log_level::ERROR, "this line must appear");
  std::string out = testing::internal::GetCapturedStdout();
  std::string err = testing::internal::GetCapturedStderr();

  EXPECT_EQ(out.find("must not appear"), std::string::npos);
  /* A warning and an error go to the error stream, so a log file of the output
   * stream is not the only place to look. */
  EXPECT_NE(err.find("this line must appear"), std::string::npos);
}
