/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mave* mave_create(int length) {
  struct mave* m;
  m = calloc(1, sizeof(*m));
  if (!m) {
    return NULL;
  }
  m->val = calloc(1, length * sizeof(*m->val));
  if (!m->val) {
    free(m);
    return NULL;
  }
  m->len = length;
  return m;
}

void mave_destroy(struct mave* m) {
  free(m->val);
  free(m);
}

int64_t mave_accumulate(struct mave* m, int64_t val) {
  m->sum = m->sum - m->val[m->index];
  m->val[m->index] = val;
  m->index = (1 + m->index) % m->len;
  m->sum = m->sum + val;
  if (m->cnt < m->len) {
    m->cnt++;
  }
  return m->sum / m->cnt;
}

void mave_reset(struct mave* m) {
  m->cnt = 0;
  m->index = 0;
  m->sum = 0;
  memset(m->val, 0, m->len * sizeof(*m->val));
}
