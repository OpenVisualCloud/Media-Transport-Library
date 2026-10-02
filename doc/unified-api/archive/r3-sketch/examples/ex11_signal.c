/* 10 §13 — shutdown from a signal handler: interrupt every waiter, then stop and destroy
   on a normal thread. */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <string.h>

#include "ex_common.h"

static mtl_instance_h g_mt; /* a value, written before the handler is installed */
static volatile sig_atomic_t g_stop;

static void on_sigint(int sig) {
  (void)sig;
  g_stop = 1;
  mtl_instance_interrupt_all(g_mt); /* AS: one eventfd write; sticky -MTL_ECANCELED */
}

int install_sigint(mtl_instance_h mt) {
  struct sigaction sa;
  g_mt = mt;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_sigint;
  sigemptyset(&sa.sa_mask);
  return sigaction(SIGINT, &sa, NULL);
}

/* worker thread: leaves its loop on -MTL_ECANCELED (sticky, so no race with g_stop) */
int worker(mtl_session_h s) {
  while (!g_stop) {
    mtl_lease_h lease;
    int ret = mtl_tx_acquire(s, &lease, NULL, NULL, MTL_SEC(1));
    if (ret == -MTL_ECANCELED) break;
    if (ret == -MTL_ETIMEDOUT) continue;
    if (ret < 0) return ex_fail("acquire", ret);
    ret = mtl_tx_submit(s, lease, NULL);
    if (ret < 0) {
      mtl_tx_release(s, lease);
      return ex_fail("submit", ret);
    }
  }
  return 0;
}

/* main thread, after joining the workers */
int shutdown_all(mtl_instance_h mt, mtl_session_h s) {
  int ret = mtl_instance_uninterrupt_all(mt); /* waits are legal again (DRAIN waits) */
  if (ret < 0) ex_fail("uninterrupt", ret);
  ret = mtl_session_stop(s, MTL_STOP_DRAIN, MTL_SEC(1));
  if (ret < 0 && ret != -MTL_ETIMEDOUT) ex_fail("stop", ret);
  ret = mtl_session_destroy(s, 0);
  if (ret < 0) ex_fail("destroy", ret);
  return mtl_instance_release(mt);
}
