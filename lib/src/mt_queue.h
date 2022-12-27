/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_QUEUE_HEAD_H_
#define _MT_LIB_QUEUE_HEAD_H_

#include <sys/queue.h>

/* Macros compatible with system's sys/queue.h */
#define MT_TAILQ_HEAD(name, type) RTE_TAILQ_HEAD(name, type)
#define MT_TAILQ_ENTRY(type) RTE_TAILQ_ENTRY(type)
#define MT_TAILQ_FOREACH(var, head, field) RTE_TAILQ_FOREACH(var, head, field)
#define MT_TAILQ_FIRST(head) RTE_TAILQ_FIRST(head)
#define MT_TAILQ_NEXT(elem, field) RTE_TAILQ_NEXT(elem, field)

#define MT_TAILQ_INSERT_TAIL(head, elem, filed) TAILQ_INSERT_TAIL(head, elem, filed)
#define MT_TAILQ_INSERT_HEAD(head, elem, filed) TAILQ_INSERT_HEAD(head, elem, filed)
#define MT_TAILQ_REMOVE(head, elem, filed) TAILQ_REMOVE(head, elem, filed)
#define MT_TAILQ_INIT(head) TAILQ_INIT(head)

#define MT_STAILQ_HEAD(name, type) RTE_STAILQ_HEAD(name, type)
#define MT_STAILQ_ENTRY(type) RTE_STAILQ_ENTRY(type)

#endif
