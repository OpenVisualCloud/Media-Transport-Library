/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#define _GNU_SOURCE
#include "app_cores.h"

#include <math.h>

#include "log.h"

/* a core this busy, shared by 2 or more threads, delays them past their frame time */
#define ST_APP_CORES_HOT_LOAD (0.65)
#define ST_APP_CORES_TARGET_LOAD (0.50)
/* threads per core at start, the density measured at full fused load is 12 to 21 */
#define ST_APP_CORES_START_THREADS (12)
#define ST_APP_CORES_WARMUP_TICKS (5)
#define ST_APP_CORES_SHRINK_TICKS (5)
/* a rebalance on the same cores has to unload the hottest shared one this much */
#define ST_APP_CORES_MIN_GAIN (0.05)

#ifndef WINDOWSENV
static cpu_set_t app_cores_main_cpus;
#endif

static int app_cores_cpu_ns(pthread_t thread, uint64_t* ns) {
#ifdef WINDOWSENV
  MTL_MAY_UNUSED(thread);
  MTL_MAY_UNUSED(ns);
  return -ENOTSUP;
#else
  clockid_t clock;
  struct timespec ts;

  if (pthread_getcpuclockid(thread, &clock) || clock_gettime(clock, &ts)) return -EIO;
  *ns = st_timespec_to_ns(&ts);
  return 0;
#endif
}

/* binding a thread that has just exited binds the caller instead (glibc uses tid 0) */
static void app_cores_save_main(void) {
#ifndef WINDOWSENV
  pthread_getaffinity_np(pthread_self(), sizeof(app_cores_main_cpus),
                         &app_cores_main_cpus);
#endif
}

static void app_cores_restore_main(void) {
#ifndef WINDOWSENV
  pthread_setaffinity_np(pthread_self(), sizeof(app_cores_main_cpus),
                         &app_cores_main_cpus);
#endif
}

static bool app_cores_get(struct st_app_context* ctx, struct st_app_cores* c) {
  unsigned int lcore;

  if (c->lcores_exhausted) return false;
  if (mtl_get_lcore(ctx->st, &lcore) < 0) {
    if (c->cnt && !c->lcores_warned) {
      warn(
          "%s, no more lcores from mtl at %d st20p app cores, pacing protection is "
          "limited\n",
          __func__, c->cnt);
      c->lcores_warned = true;
    }
    c->lcores_exhausted = true;
    return false;
  }
  c->lcores[c->cnt++] = lcore;
  return true;
}

static int app_cores_pin(struct st_app_context* ctx, struct st_app_cores* c,
                         struct st_app_core_thread* t, int core) {
  if (mtl_bind_to_lcore(ctx->st, t->thread, c->lcores[core]) < 0) {
    if (!c->bind_warned)
      warn("%s, bind to lcore %u fail, the thread stays where it is\n", __func__,
           c->lcores[core]);
    c->bind_warned = true;
    t->fixed = true;
    return -EIO;
  }
  t->core = core;
  return 0;
}

static void app_cores_swap(struct st_app_cores* c, int a, int b) {
  unsigned int lcore = c->lcores[a];

  c->lcores[a] = c->lcores[b];
  c->lcores[b] = lcore;
  for (int i = 0; i < c->thread_cnt; i++) {
    if (c->threads[i].core == a)
      c->threads[i].core = b;
    else if (c->threads[i].core == b)
      c->threads[i].core = a;
  }
}

/* the cores of fixed threads first, so a plan on k >= fixed_cores keeps them */
static void app_cores_front_fixed(struct st_app_cores* c) {
  struct st_app_core_thread* t;

  c->fixed_cores = 0;
  for (int i = 0; i < c->thread_cnt; i++) {
    t = &c->threads[i];
    if (!t->tracked || !t->fixed || t->core < c->fixed_cores) continue;
    app_cores_swap(c, t->core, c->fixed_cores);
    c->fixed_cores++;
  }
}

/* whether a core of 2 or more threads placed as assign[] on k cores is hot */
static bool app_cores_eval(struct st_app_cores* c, int k, const int* assign, double* max,
                           double* max_shared) {
  *max = 0;
  *max_shared = 0;
  for (int i = 0; i < k; i++) {
    c->core_load[i] = 0;
    c->core_threads[i] = 0;
  }
  for (int i = 0; i < c->thread_cnt; i++) {
    if (!c->threads[i].tracked || assign[i] < 0) continue;
    c->core_load[assign[i]] += c->threads[i].load;
    c->core_threads[assign[i]]++;
  }
  for (int i = 0; i < k; i++) {
    *max = ST_MAX(*max, c->core_load[i]);
    if (c->core_threads[i] > 1) *max_shared = ST_MAX(*max_shared, c->core_load[i]);
  }
  return *max_shared > ST_APP_CORES_HOT_LOAD;
}

/* order[]: the threads, heaviest first */
static void app_cores_sort(struct st_app_cores* c) {
  for (int i = 0; i < c->thread_cnt; i++) {
    int idx = i;

    while (idx > 0 && c->threads[c->order[idx - 1]].load < c->threads[i].load) {
      c->order[idx] = c->order[idx - 1];
      idx--;
    }
    c->order[idx] = i;
  }
}

/* plan[]: in order[], each tracked thread onto the least loaded of k cores, a tie keeps
 * a thread where it is; a fixed thread stays, k >= fixed_cores */
static void app_cores_lpt(struct st_app_cores* c, int k) {
  struct st_app_core_thread* t;

  for (int i = 0; i < k; i++) c->core_load[i] = 0;
  for (int i = 0; i < c->thread_cnt; i++) {
    t = &c->threads[i];
    c->plan[i] = t->core;
    if (t->tracked && t->fixed && t->core >= 0) c->core_load[t->core] += t->load;
  }
  for (int i = 0; i < c->thread_cnt; i++) {
    int idx = c->order[i];
    int best;

    t = &c->threads[idx];
    if (!t->tracked || t->fixed) continue;
    best = t->core >= 0 && t->core < k ? t->core : 0;
    for (int j = 0; j < k; j++)
      if (c->core_load[j] < c->core_load[best]) best = j;
    c->core_load[best] += t->load;
    c->plan[idx] = best;
  }
}

/* the fewest cores in [from, to) whose plan has no hot core, else to */
static int app_cores_fit(struct st_app_cores* c, int from, int to) {
  double max, max_shared;

  for (int k = from; k < to; k++) {
    app_cores_lpt(c, k);
    if (!app_cores_eval(c, k, c->plan, &max, &max_shared)) return k;
  }
  return to;
}

static void app_cores_apply(struct st_app_context* ctx, struct st_app_cores* c) {
  struct st_app_core_thread* t;

  app_cores_save_main();
  for (int i = 0; i < c->thread_cnt; i++) {
    t = &c->threads[i];
    if (t->tracked && !t->fixed && c->plan[i] != t->core)
      app_cores_pin(ctx, c, t, c->plan[i]);
  }
  app_cores_restore_main();
}

static void app_cores_add_thread(struct st_app_cores* c, pthread_t thread) {
  if (!thread) return;
  c->threads[c->thread_cnt].thread = thread;
  c->threads[c->thread_cnt].core = -1;
  c->thread_cnt++;
}

int st_app_cores_start(struct st_app_context* ctx) {
  struct st_app_cores* c = &ctx->st20p_cores;
  struct st_app_core_thread* t;
  int n = ctx->tx_st20p_session_cnt + ctx->rx_st20p_session_cnt;
  bool no_clock = false;
  int pinnable = 0, want, ret;

  if (!c->requested || !n) return 0;

  c->threads = st_app_zmalloc(sizeof(*c->threads) * n);
  c->lcores = st_app_zmalloc(sizeof(*c->lcores) * n);
  c->core_load = st_app_zmalloc(sizeof(*c->core_load) * n);
  c->core_threads = st_app_zmalloc(sizeof(*c->core_threads) * n);
  c->plan = st_app_zmalloc(sizeof(*c->plan) * n);
  c->order = st_app_zmalloc(sizeof(*c->order) * n);
  if (!c->threads || !c->lcores || !c->core_load || !c->core_threads || !c->plan ||
      !c->order) {
    err("%s, malloc fail\n", __func__);
    st_app_cores_free(ctx);
    return -ENOMEM;
  }
  for (int i = 0; ctx->tx_st20p_sessions && i < ctx->tx_st20p_session_cnt; i++)
    app_cores_add_thread(c, ctx->tx_st20p_sessions[i].st20p_app_thread);
  for (int i = 0; ctx->rx_st20p_sessions && i < ctx->rx_st20p_session_cnt; i++)
    app_cores_add_thread(c, ctx->rx_st20p_sessions[i].st20p_app_thread);
  if (!c->thread_cnt) return 0;

  /* measure first: a thread that has exited is never pinned or counted */
  c->last_ns = st_app_get_monotonic_time();
  for (int i = 0; i < c->thread_cnt; i++) {
    t = &c->threads[i];
    ret = app_cores_cpu_ns(t->thread, &t->cpu_ns);
    if (ret == -ENOTSUP) no_clock = true;
    t->tracked = ret >= 0;
    if (t->tracked) pinnable++;
  }
  if (no_clock) pinnable = c->thread_cnt;
  if (no_clock && c->requested < 0) {
    info("%s, no thread cpu time to size by, the st20p app threads stay unpinned\n",
         __func__);
    return 0;
  }

  if (!pinnable) return 0;
  if (c->requested > 0)
    want = ST_MIN(c->requested, pinnable);
  else
    want = (pinnable + ST_APP_CORES_START_THREADS - 1) / ST_APP_CORES_START_THREADS;
  while (c->cnt < want) {
    if (!app_cores_get(ctx, c)) break;
  }
  if (!c->cnt) {
    warn("%s, no lcore from mtl, the st20p app threads stay unpinned\n", __func__);
    return 0;
  }

  /* pinned from here as the threads already run, and the tick moves them later */
  app_cores_save_main();
  for (int i = 0, slot = 0; i < c->thread_cnt; i++) {
    t = &c->threads[i];
    if (!t->tracked && !no_clock) continue;
    app_cores_pin(ctx, c, t, slot++ % c->cnt);
  }
  app_cores_restore_main();
  info("%s, st20p app cores %d for %d threads\n", __func__, c->cnt, c->thread_cnt);
  return 0;
}

/* never once session uinit starts: pthread_getcpuclockid() on a joined thread crashes */
void st_app_cores_tick(struct st_app_context* ctx) {
  struct st_app_cores* c = &ctx->st20p_cores;
  uint64_t now = st_app_get_monotonic_time();
  struct st_app_core_thread* t;
  int old = c->cnt, need, k;
  double max_shared, plan_max, plan_shared;
  uint64_t cpu_ns = 0;

  if (!c->cnt || now <= c->last_ns) return;

  c->load = 0;
  for (int i = 0; i < c->thread_cnt; i++) {
    t = &c->threads[i];
    if (t->tracked && app_cores_cpu_ns(t->thread, &cpu_ns) < 0) { /* exited */
      t->tracked = false;
      t->core = -1;
    }
    c->plan[i] = t->core;
    if (!t->tracked) continue;
    t->load = (double)(cpu_ns - t->cpu_ns) / (double)(now - c->last_ns);
    t->cpu_ns = cpu_ns;
    if (t->core >= 0) c->load += t->load;
  }
  c->last_ns = now;
  c->ticks++;
  app_cores_front_fixed(c);
  for (int i = 0; i < c->thread_cnt; i++) c->plan[i] = c->threads[i].core;

  need = ST_MAX(1, (int)ceil(c->load / ST_APP_CORES_TARGET_LOAD));
  if (app_cores_eval(c, c->cnt, c->plan, &c->max_core_load, &max_shared)) {
    /* growth is safety, from the first tick; the current cores are tried first */
    c->shrink_ticks = 0;
    app_cores_sort(c);
    if (c->requested < 0 && !c->lcores_exhausted) {
      k = app_cores_fit(c, ST_MAX(need, c->cnt), c->thread_cnt);
      while (c->cnt < k) {
        if (!app_cores_get(ctx, c)) break;
      }
    }
    k = c->cnt;
    app_cores_lpt(c, k);
    if (k == old && app_cores_eval(c, k, c->plan, &plan_max, &plan_shared) &&
        plan_shared > max_shared - ST_APP_CORES_MIN_GAIN)
      return;
  } else if (c->requested < 0 && need < c->cnt && c->ticks > ST_APP_CORES_WARMUP_TICKS) {
    if (++c->shrink_ticks < ST_APP_CORES_SHRINK_TICKS) return;
    c->shrink_ticks = 0;
    /* no hot core on the fewer cores, else the next tick grows again */
    app_cores_sort(c);
    k = app_cores_fit(c, ST_MAX(need, c->fixed_cores), c->cnt);
    if (k == c->cnt) return;
  } else {
    c->shrink_ticks = 0;
    return;
  }

  app_cores_apply(ctx, c);
  /* a thread that failed to move fixes its core, which is kept */
  app_cores_front_fixed(c);
  for (int i = 0; i < c->thread_cnt; i++) {
    if (c->threads[i].core < k) continue;
    app_cores_swap(c, c->threads[i].core, k);
    k++;
  }
  if (k < c->cnt) {
    for (int i = k; i < c->cnt; i++) mtl_put_lcore(ctx->st, c->lcores[i]);
    c->cnt = k;
    c->lcores_exhausted = false;
  }
  for (int i = 0; i < c->thread_cnt; i++) c->plan[i] = c->threads[i].core;
  app_cores_eval(c, c->cnt, c->plan, &c->max_core_load, &max_shared);
  if (c->cnt != old)
    info("%s, st20p app cores %d -> %d, load %.1f cores, max core %.0f%%\n", __func__,
         old, c->cnt, c->load, c->max_core_load * 100);
}

void st_app_cores_stat(struct st_app_context* ctx) {
  struct st_app_cores* c = &ctx->st20p_cores;

  if (!c->cnt) return;
  info("%s, st20p app cores %d, load %.1f cores, max core %.0f%%\n", __func__, c->cnt,
       c->load, c->max_core_load * 100);
}

void st_app_cores_free(struct st_app_context* ctx) {
  struct st_app_cores* c = &ctx->st20p_cores;

  for (int i = 0; i < c->cnt; i++) mtl_put_lcore(ctx->st, c->lcores[i]);
  c->cnt = 0;
  st_app_free(c->threads);
  c->threads = NULL;
  st_app_free(c->lcores);
  c->lcores = NULL;
  st_app_free(c->core_load);
  c->core_load = NULL;
  st_app_free(c->core_threads);
  c->core_threads = NULL;
  st_app_free(c->plan);
  c->plan = NULL;
  st_app_free(c->order);
  c->order = NULL;
}
