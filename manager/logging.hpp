/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

enum class log_level { DEBUG, INFO, WARNING, ERROR };

class logger {
 public:
  static void log(log_level level, const std::string& message) {
    if (level >= log_level_min) {
      print_log_header(level);
      std::cout << message << std::endl;
    }
  }

  static void set_log_level(log_level level) {
    log_level_min = level;
  }

 private:
  static log_level log_level_min;

  static void print_log_header(log_level level) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const char* level_str = get_log_level_string(level);

    std::cout << "[" << std::put_time(std::localtime(&now), "%F %T") << "] [" << level_str
              << "] ";
  }

  static const char* get_log_level_string(log_level level) {
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
};

/* Initialize the log level threshold */
log_level logger::log_level_min = log_level::DEBUG;

#endif
