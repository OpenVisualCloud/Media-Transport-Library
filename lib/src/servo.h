/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _SERVO_H_
#define _SERVO_H_

#define HWTS_KP 0.7
#define HWTS_KI 0.3

#define SWTS_KP 0.1
#define SWTS_KI 0.001

#define NSEC_PER_SEC 1000000000

enum servo_state {
  /**
   * The servo is not yet ready to track the master clock.
   */
  SERVO_UNLOCKED,
  /**
   * The is ready to track and requests a clock jump to
   * immediately correct the estimated offset.
   */
  SERVO_JUMP,
  /**
   * The servo is tracking the master clock.
   */
  SERVO_LOCKED,
};

struct pi_servo {
  double offset[2];
  double local[2];
  double drift;
  double maxppb;
  double kp;
  double ki;
  double max_offset;
  int count;
};

/**
 * Create a new instance of a clock servo.
 * @param type    The type of the servo to create.
 * @param fadj    The clock's current adjustment in parts per billion.
 * @param max_ppb The absolute maxinum adjustment allowed by the clock
 *                in parts per billion. The clock servo will clamp its
 *                output according to this limit.
 * @param sw_ts   Indicates that software time stamping will be used,
 *                and the servo should use more aggressive filtering.
 * @return A pointer to a new servo on success, NULL otherwise.
 */
struct pi_servo* pi_servo_create(int fadj, int max_ppb, int sw_ts);

/**
 * Destroy an instance of a clock servo.
 * @param servo Pointer to a servo obtained via @ref servo_create().
 */
void pi_destroy(struct pi_servo* s);

/**
 * Feed a sample into a clock servo.
 * @param servo     Pointer to a servo obtained via @ref servo_create().
 * @param offset    The estimated clock offset in nanoseconds.
 * @param local_ts  The local time stamp of the sample in nanoseconds.
 * @param state     Returns the servo's state.
 * @return The clock adjustment in parts per billion.
 */
double pi_sample(struct pi_servo* s, double offset, double local_ts,
                 enum servo_state* state);

#endif /* _SERVO_H_ */
