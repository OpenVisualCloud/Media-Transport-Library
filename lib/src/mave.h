/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MAVE_H_
#define _MAVE_H_

#include <stdint.h>

struct mave {
  int cnt;
  int len;
  int index;
  int64_t sum;
  int64_t* val;
};

struct mave* mave_create(int length);

void mave_destroy(struct mave* m);

int64_t mave_accumulate(struct mave* m, int64_t val);

void mave_reset(struct mave* m);

#endif /* _MAVE_H_ */
