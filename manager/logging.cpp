/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "logging.hpp"

#include <ctime>
#include <iomanip>
#include <iostream>

log_level logger::log_level_min = log_level::INFO;

const char* logger::level_string(log_level level) {
  switch (level) {
    case log_level::DEBUG:
      return "DEBUG";
    case log_level::INFO:
      return "INFO";
    case log_level::WARNING:
      return "WARNING";
    case log_level::ERROR:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

void logger::set_log_level(log_level level) {
  log_level_min = level;
}

bool logger::set_log_level(const std::string& name) {
  if (name == "debug")
    log_level_min = log_level::DEBUG;
  else if (name == "info")
    log_level_min = log_level::INFO;
  else if (name == "warning")
    log_level_min = log_level::WARNING;
  else if (name == "error")
    log_level_min = log_level::ERROR;
  else
    return false;

  return true;
}

log_level logger::get_log_level() {
  return log_level_min;
}

void logger::log(log_level level, const std::string& message) {
  if (level < log_level_min) return;

  /* localtime() keeps its result in a shared buffer, so use the _r form. */
  std::time_t now = std::time(nullptr);
  std::tm tm_buf = {};
  localtime_r(&now, &tm_buf);

  /* Errors and warnings go to stderr so that a caller can separate them from
   * the running log. Both streams reach the journal under systemd. */
  std::ostream& out = (level >= log_level::WARNING) ? std::cerr : std::cout;

  out << "[" << std::put_time(&tm_buf, "%F %T") << "] [" << level_string(level) << "] "
      << message << std::endl;
}
