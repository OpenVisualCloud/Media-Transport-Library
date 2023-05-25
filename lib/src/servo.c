/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "servo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

double configured_pi_kp = 0.0;
double configured_pi_ki = 0.0;
double configured_pi_offset = 0.0;

void pi_destroy(struct pi_servo* s) { free(s); }

double pi_sample(struct pi_servo* s, double offset, double local_ts,
                 enum servo_state* state) {
  double ki_term, ppb = 0.0;

  switch (s->count) {
    case 0:
      s->offset[0] = offset;
      s->local[0] = local_ts;
      *state = SERVO_UNLOCKED;
      s->count = 1;
      break;
    case 1:
      s->offset[1] = offset;
      s->local[1] = local_ts;
      *state = SERVO_UNLOCKED;
      s->count = 2;
      break;
    case 2:
      s->drift += (s->offset[1] - s->offset[0]) / (s->local[1] - s->local[0]);
      *state = SERVO_UNLOCKED;
      s->count = 3;
      break;
    case 3:
      *state = SERVO_JUMP;
      s->count = 4;
      break;
    case 4:
      /*
       * reset the clock servo when offset is greater than the max
       * offset value. Note that the clock jump will be performed in
       * step 3, so it is not necessary to have clock jump
       * immediately. This allows re-calculating drift as in initial
       * clock startup.
       */
      if (s->max_offset && (s->max_offset < fabs(offset))) {
        *state = SERVO_UNLOCKED;
        s->count = 0;
        break;
      }

      ki_term = s->ki * offset;
      ppb = s->kp * offset + s->drift + ki_term;
      if (ppb < -s->maxppb) {
        ppb = -s->maxppb;
      } else if (ppb > s->maxppb) {
        ppb = s->maxppb;
      } else {
        s->drift += ki_term;
      }
      *state = SERVO_LOCKED;
      break;
  }

  return ppb;
}

struct pi_servo* pi_servo_create(int fadj, int max_ppb, int sw_ts) {
  struct pi_servo* s;

  s = calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->drift = fadj;
  s->maxppb = max_ppb;

  if (configured_pi_kp && configured_pi_ki) {
    s->kp = configured_pi_kp;
    s->ki = configured_pi_ki;
  } else if (sw_ts) {
    s->kp = SWTS_KP;
    s->ki = SWTS_KI;
  } else {
    s->kp = HWTS_KP;
    s->ki = HWTS_KI;
  }

  if (configured_pi_offset > 0.0) {
    s->max_offset = configured_pi_offset * NSEC_PER_SEC;
  } else {
    s->max_offset = 0.0;
  }

  return s;
}
