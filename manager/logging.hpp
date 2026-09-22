/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <string>

enum class log_level { DEBUG, INFO, WARNING, ERROR };

class logger {
 public:
  static void log(log_level level, const std::string& message);

  static void set_log_level(log_level level);

  /**
   * Set the level from a name: "debug", "info", "warning" or "error".
   *
   * @return true when the name is one of those, false when it is not.
   */
  static bool set_log_level(const std::string& name);

  static log_level get_log_level();

  static const char* level_string(log_level level);

 private:
  static log_level log_level_min;
};

#endif
