/* ex16 — a service in a Kubernetes pod: SIGTERM ends every wait, the instance shuts down
   network first within the grace period and leaves a report, the probes read one
   lock-free call, and the node's PTP daemon tells MTL about the clock. Needs: MS3. */
#define _POSIX_C_SOURCE 200809L
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_sync.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "ex_common.h"

static mtl_instance_h g_mt; /* set before the signals are unblocked */
static volatile sig_atomic_t g_signals;
static struct timespec g_term; /* when the first signal came */

static void on_term(int sig) {
  (void)sig;
  if (g_signals) {
    mtl_instance_abort(g_mt); /* the second signal: stop at the next packet */
    return;
  }
  g_signals = 1;
  clock_gettime(CLOCK_MONOTONIC, &g_term); /* async-signal-safe */
  mtl_instance_interrupt(g_mt, 1);         /* every wait returns -MTL_ECANCELED, sticky */
}

/* Called first in main(), before open and before any thread: the signals stay blocked
   (pending, never lost, even as PID 1) until the handle exists. The library installs no
   handler (mtl.h R8). */
int block_signals(sigset_t* set) {
  sigemptyset(set);
  sigaddset(set, SIGTERM);
  sigaddset(set, SIGINT);
  return sigprocmask(SIG_BLOCK, set, NULL);
}
/* After open, on the main thread; workers created before keep the signals blocked. */
int install_handlers(mtl_instance_h mt, const sigset_t* set) {
  struct sigaction sa;
  g_mt = mt;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_term;
  sa.sa_mask = *set;
  if (sigaction(SIGTERM, &sa, NULL) < 0 || sigaction(SIGINT, &sa, NULL) < 0) return -1;
  return sigprocmask(SIG_UNBLOCK, set, NULL);
}

/* A worker leaves on -MTL_ECANCELED: sticky, so it cannot miss the signal. */
int worker(mtl_session_h s) {
  struct mtl_unit u;
  int ret;
  MTL_INIT(&u);
  while ((ret = mtl_tx_acquire(s, &u, MTL_SEC(1))) == 0 || ret == -MTL_EAGAIN)
    if (ret == 0 && (ret = mtl_tx_submit(s, &u)) < 0) break;
  return ret == -MTL_ECANCELED ? 0 : ex_fail("worker", ret);
}

/* The HTTP probe handlers. Before open returns, answer 503 for all three. Startup and
   liveness fail only on what a restart can fix; readiness also on no link, no time and
   shutdown. After the shutdown began, liveness stays 200 until the process exits. */
enum probe { PROBE_STARTUP, PROBE_LIVENESS, PROBE_READINESS };
int probe_status(mtl_instance_h mt, enum probe p) {
  int flags = mtl_instance_get_health(mt, NULL, 0);
  if (flags == -MTL_ESHUTDOWN) return p == PROBE_READINESS ? 503 : 200;
  if (flags < 0) return 503;
  int bad = p == PROBE_READINESS ? MTL_HEALTH_READINESS : MTL_HEALTH_LIVENESS;
  return (flags & bad) ? 503 : 200;
}

/* The main thread, after joining the workers. grace_ns: terminationGracePeriodSeconds
   minus any preStop time; the budget counts from the signal, not from now. */
int shutdown_all(mtl_instance_h mt, int64_t grace_ns) {
  struct mtl_shutdown_report r;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t spent =
      (int64_t)(now.tv_sec - g_term.tv_sec) * MTL_SEC(1) + (now.tv_nsec - g_term.tv_nsec);
  int64_t budget = grace_ns - MTL_SEC(2) - spent; /* 2 s for our own exit */
  if (budget < MTL_MS(100)) budget = MTL_MS(100);
  memset(&r, 0, sizeof(r));
  /* this application owns main(): shut down for every component of the process */
  int ret = mtl_instance_shutdown(mt, MTL_SHUTDOWN_ALL_REFERENCES, budget, &r, sizeof(r));
  FILE* f = fopen("/dev/termination-log", "w"); /* the pod's terminationMessagePath */
  if (f) {
    fprintf(f, "%s\n", r.summary);
    fclose(f);
  }
  return ret < 0 ? ret : 0; /* 1: devices stopped; held memory goes at exit */
}

/* The node's ptp4l and phc2sys discipline the clock MTL reads (time_source AUTO:
   CLOCK_TAI), but MTL cannot see the grandmaster: pass what pmc reports, and locked = 0
   when the node lost it, so readiness and the time.* stats follow. The result: the time
   flags (MTL_TIMEF_ESTIMATED: not locked to PTP). In a pod without CPUs to pin, the
   instance also sets MTL_INSTANCE_TASKLET_THREAD. */
int set_time_reference(mtl_instance_h mt, const uint8_t gmid[8], uint8_t clock_class,
                       int locked) {
  struct mtl_time_reference ref;
  int64_t tai;
  MTL_INIT(&ref);
  memcpy(ref.gmid, gmid, sizeof(ref.gmid));
  ref.clock_class = clock_class; /* 220, 228: ARB, MTL_TIMEF_ARB_TIMESCALE */
  ref.locked = locked ? 1u : 0u;
  int ret = mtl_time_set_reference(mt, 0, &ref); /* port 0: every port */
  return ret < 0 ? ret : mtl_time_now(mt, &tai, NULL, NULL);
}
