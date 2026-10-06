/* Real-kernel stress test of the wait protocol (core.md §6) on this host (x86, Linux):
 * futex (FUTEX_WAIT_BITSET / FUTEX_WAKE_BITSET), eventfd, epoll LT or EPOLLONESHOT.
 * One object, lanes 0 and 1. Threads per round:
 *   2 producers (direct-context EVENT + WAKE_NOW, random lanes),
 *   2 event-loop threads sharing the handle (sweep both lanes with DP misses, then
 * epoll), 1 WT waiter on lane 0 (futex), 1 poller that makes one-shot DP calls on lane 0
 * and never sleeps on the handle (N3/N9). Each round produces UNITS units; the round
 * fails with LOST if units stay unconsumed for 1 s while every consumer is blocked, and
 * with SPIN if, once everything is consumed and the producers and poller are done, the
 * epoll threads return more than 8 times in 50 ms. Mutants (-D): NO_REPOST (drop D8),
 * NO_E2 (drop the EVENT fence), NO_HSIG (drain only when the call cleared the last bit,
 * as the review's (h)), NO_RESIGNAL (drop R6). Build: cc -O2 -pthread stress.c ; run:
 * ./a.out rounds [oneshot] */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define UNITS 400
#define HB(l) (1ull << (16 * (l) + 15))
#define ONE(l) (1ull << (16 * (l)))
#define WCM(l) (0x7fffull << (16 * (l)))

static _Atomic uint64_t armed;
static _Atomic uint32_t wseq, h_pend, h_sig, h_done, h_seen, stop_flag;
static _Atomic int64_t R[2];
static _Atomic long consumed, produced, ep_returns;
static _Atomic int blocked; /* consumers currently blocked (epoll or futex) */
static int efd, oneshot;
static __thread unsigned seed;

static void Y(void) {
  unsigned r = rand_r(&seed);
  if ((r & 15) == 0)
    sched_yield();
  else if ((r & 15) == 1)
    for (volatile int i = 0; i < (int)(r >> 20) % 200; i++) {
    }
}

static long futex(_Atomic uint32_t* a, int op, uint32_t v, uint32_t bits) {
  return syscall(SYS_futex, (uint32_t*)a, op | FUTEX_PRIVATE_FLAG, v, NULL, NULL, bits);
}

static int attempt(int l) {
  int64_t v = atomic_load(&R[l]);
  while (v > 0)
    if (atomic_compare_exchange_weak(&R[l], &v, v - 1)) {
      atomic_fetch_add(&consumed, 1);
      return 1;
    }
  return 0;
}
static int ready(int l) {
  return atomic_load_explicit(&R[l], memory_order_acquire) > 0;
}

static void sig_(void) {
  atomic_fetch_add_explicit(&h_sig, 1, memory_order_release); /* S3 */
  Y();
  uint64_t one = 1;
  if (write(efd, &one, 8) != 8) abort(); /* S4 */
  Y();
  atomic_fetch_add_explicit(&h_done, 1, memory_order_release); /* S5 */
}
static void post(uint32_t l) {
  if (atomic_fetch_or(&h_pend, l) == 0) {
    Y();
    sig_();
  }
}
static void wake_now(uint32_t F) {
  uint64_t hb = 0, wc = 0;
  for (int l = 0; l < 2; l++)
    if (F & (1u << l)) hb |= HB(l), wc |= WCM(l);
  uint64_t a = atomic_load_explicit(&armed, memory_order_acquire); /* W1 */
  if (a & hb) a = atomic_fetch_and(&armed, ~hb);                   /* W2 */
  Y();
  if (a & wc) { /* W3 */
    atomic_fetch_add_explicit(&wseq, 1, memory_order_release);
    Y();
    futex(&wseq, FUTEX_WAKE_BITSET, INT_MAX, F);
  }
  uint32_t hf = 0;
  for (int l = 0; l < 2; l++)
    if ((F & (1u << l)) && (a & HB(l))) hf |= 1u << l;
  if (hf) post(hf); /* W5 */
}
static void event(int l) {
  atomic_fetch_add(&produced, 1);
  atomic_fetch_add_explicit(&R[l], 1, memory_order_release); /* E1 */
#ifndef NO_E2
  atomic_thread_fence(memory_order_seq_cst); /* E2 */
#endif
  Y();
  uint64_t a = atomic_load_explicit(&armed, memory_order_relaxed); /* E3 */
  if (a & (HB(l) | WCM(l))) wake_now(1u << l);
}
static void drain(void) {
  uint32_t d = atomic_load_explicit(&h_done, memory_order_acquire); /* R2 */
  uint64_t v;
  Y();
  if (read(efd, &v, 8) < 0 && errno != EAGAIN) abort();    /* R3 */
  atomic_store_explicit(&h_seen, d, memory_order_release); /* R4 */
  atomic_thread_fence(memory_order_seq_cst);               /* R5 */
#ifndef NO_RESIGNAL
  if (atomic_load_explicit(&h_pend, memory_order_relaxed)) sig_(); /* R6 */
#endif
}
/* DP call on lane l: 1 = got a unit, 0 = -EAGAIN */
static int dp(int l) {
  if (attempt(l)) return 1; /* D2, D3 */
  uint32_t b = 1u << l, took = 0;
  uint32_t p = atomic_load_explicit(&h_pend, memory_order_acquire); /* K1 */
  if (p & b) {
    uint32_t old = atomic_fetch_and_explicit(&h_pend, ~b, memory_order_acq_rel); /* K2 */
    took = old & b;
    p = old & ~b;
  }
  Y();
#ifdef NO_HSIG
  if (took && p == 0) drain();
#else
  if (p == 0 && atomic_load_explicit(&h_sig, memory_order_acquire) !=
                    atomic_load_explicit(&h_seen, memory_order_relaxed))
    drain(); /* K3 */
#endif
  uint64_t a = atomic_load(&armed);                 /* H1 */
  if (!(a & HB(l))) atomic_fetch_or(&armed, HB(l)); /* H2 */
  atomic_thread_fence(memory_order_seq_cst);        /* H3 */
  Y();
  int r = ready(l) ? attempt(l) : 0; /* D7 */
#ifndef NO_REPOST
  if (took && r) post(took); /* D8 */
#endif
  return r;
}

static void* producer(void* arg) {
  seed = (unsigned)(uintptr_t)arg * 7919u + (unsigned)time(NULL);
  for (int i = 0; i < UNITS / 2; i++) {
    event(rand_r(&seed) & 1);
    if ((rand_r(&seed) & 3) == 0) usleep(rand_r(&seed) % 50);
  }
  return NULL;
}
static void* evloop(void* arg) {
  seed = (unsigned)(uintptr_t)arg * 104729u + (unsigned)time(NULL);
  int ep = (intptr_t)arg >> 8;
  struct epoll_event ev;
  for (;;) {
    for (int l = 0; l < 2; l++)
      while (dp(l)) {
      }
    if (oneshot) {
      ev.events = EPOLLIN | EPOLLONESHOT;
      ev.data.u32 = 0;
      epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev);
    }
    if (atomic_load(&stop_flag)) return NULL;
    atomic_fetch_add(&blocked, 1);
    int n = epoll_wait(ep, &ev, 1, -1);
    atomic_fetch_sub(&blocked, 1);
    if (n > 0) atomic_fetch_add(&ep_returns, 1);
  }
}
static void* wtwaiter(void* arg) {
  seed = (unsigned)(uintptr_t)arg + (unsigned)time(NULL);
  for (;;) {
    if (atomic_load(&stop_flag)) return NULL;
    if (attempt(0)) continue;                                       /* T4 */
    atomic_fetch_add(&armed, ONE(0));                               /* T5 */
    atomic_thread_fence(memory_order_seq_cst);                      /* T6 */
    uint32_t v = atomic_load_explicit(&wseq, memory_order_acquire); /* T7 */
    Y();
    if (ready(0) || atomic_load(&stop_flag)) { /* T8 */
      atomic_fetch_sub(&armed, ONE(0));
      continue;
    }
    atomic_fetch_add(&blocked, 1);
    futex(&wseq, FUTEX_WAIT_BITSET, v, 1u); /* T10 */
    atomic_fetch_sub(&blocked, 1);
    atomic_fetch_sub(&armed, ONE(0)); /* T11 */
  }
}
static _Atomic int poller_done;
static void* poller(void* arg) {
  seed = (unsigned)(uintptr_t)arg + 31u * (unsigned)time(NULL);
  for (int i = 0; i < UNITS / 4; i++) {
    dp(0);
    usleep(rand_r(&seed) % 40);
  }
  atomic_store(&poller_done, 1);
  return NULL;
}

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char** argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 100;
  oneshot = argc > 2 && atoi(argv[2]);
  long lost = 0, spin = 0;
  for (int r = 0; r < rounds; r++) {
    armed = 0;
    wseq = h_pend = h_sig = h_done = h_seen = stop_flag = 0;
    R[0] = R[1] = 0;
    consumed = produced = ep_returns = 0;
    blocked = 0;
    poller_done = 0;
    efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int ep[2];
    pthread_t th[7];
    for (int i = 0; i < 2; i++) {
      ep[i] = epoll_create1(0);
      struct epoll_event ev = {.events = EPOLLIN | (oneshot ? EPOLLONESHOT : 0)};
      epoll_ctl(ep[i], EPOLL_CTL_ADD, efd, &ev);
    }
    if (oneshot) ep[1] = ep[0]; /* EPOLLONESHOT: both threads on one shared epoll set */
    pthread_create(&th[0], NULL, evloop, (void*)(intptr_t)((ep[0] << 8) | 1));
    pthread_create(&th[1], NULL, evloop, (void*)(intptr_t)((ep[1] << 8) | 2));
    pthread_create(&th[2], NULL, wtwaiter, (void*)(intptr_t)3);
    pthread_create(&th[3], NULL, poller, (void*)(intptr_t)4);
    usleep(200);
    pthread_create(&th[4], NULL, producer, (void*)(intptr_t)5);
    pthread_create(&th[5], NULL, producer, (void*)(intptr_t)6);
    pthread_join(th[4], NULL);
    pthread_join(th[5], NULL);
    pthread_join(th[3], NULL);
    double t0 = now();
    int bad = 0;
    while (atomic_load(&consumed) < atomic_load(&produced)) {
      if (now() - t0 > 1.0) {
        bad = 1;
        break;
      }
      usleep(100);
    }
    if (bad) {
      lost++;
      if (lost <= 3)
        printf(
            "round %d LOST: produced %ld consumed %ld R=%ld/%ld armed=%#llx h_pend=%u "
            "sig=%u done=%u "
            "seen=%u blocked=%d\n",
            r, (long)produced, (long)consumed, (long)R[0], (long)R[1],
            (unsigned long long)armed, h_pend, h_sig, h_done, h_seen, blocked);
    } else {
      long e0 = atomic_load(&ep_returns);
      usleep(50000);
      long e1 = atomic_load(&ep_returns);
      if (e1 - e0 > 8) {
        spin++;
        if (spin <= 3)
          printf("round %d SPIN: %ld epoll returns in 50 ms after the end\n", r, e1 - e0);
      }
    }
    atomic_store(&stop_flag, 1);
    uint64_t one = 1;
    if (write(efd, &one, 8) != 8) abort();
    for (int k = 0; k < 100; k++) {
      atomic_fetch_add(&wseq, 1);
      futex(&wseq, FUTEX_WAKE_BITSET, INT_MAX, FUTEX_BITSET_MATCH_ANY);
      struct epoll_event ev = {.events = EPOLLIN | (oneshot ? EPOLLONESHOT : 0)};
      if (oneshot) epoll_ctl(ep[0], EPOLL_CTL_MOD, efd, &ev);
      usleep(100);
    }
    for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    close(ep[0]);
    if (!oneshot) close(ep[1]);
    close(efd);
  }
  printf("rounds=%d units/round=%d oneshot=%d LOST=%ld SPIN=%ld\n", rounds, UNITS,
         oneshot, lost, spin);
  return lost || spin;
}
