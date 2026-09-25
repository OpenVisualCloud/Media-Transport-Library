# 0009 net/iavf: fix interrupt callback race on close

`iavf_dev_close()` unregisters its interrupt callback with `rte_intr_callback_unregister()` and ignores the result. While the EAL interrupt thread is running that callback, EAL returns `-EAGAIN` and keeps the callback registered.

The patch switches `iavf_dev_close()` and the `iavf_dev_init()` error path to `rte_intr_callback_unregister_sync()`, which waits for the callback to return. `ice_dev_close()` already does this.

## What breaks

The failure comes at process exit, after the test has passed. MTL closes its ports in `mtl_uninit()`: `mt_dev_if_uinit()` calls `rte_eth_dev_close()`, and `mt_dev_uinit()` then calls `rte_eal_cleanup()`. One of two signatures follows:

- **SIGSEGV** in `mt_dev_if_uinit()` / port close. The interrupt thread resumes `iavf_dev_interrupt_handler()` after `rte_eth_dev_release_port()` has set `dev->data = NULL`, and `iavf_handle_virtchnl_msg()` dereferences it.
- **SIGABRT** at `rte_eal_cleanup()`:

  ```text
  EAL: PANIC in eal_intr_thread_main():
  Error adding fd N epoll_ctl, Bad file descriptor
  ```

  The callback returned, but its source stayed in the EAL list. `pci_vfio_unmap_resource_primary()` closes the MSI-X eventfd, and the next rebuild of the epoll set fails on it.

## Root cause

1. Close sends virtchnl commands (RSS delete, promiscuous mode, flow flush). For each, `iavf_wait_for_msg()` polls every 1 ms until the interrupt thread clears `vf->pend_cmd`.
2. The PF answers. The EAL interrupt thread marks the vector 0 source active and calls `iavf_dev_interrupt_handler()`.
3. The handler clears `pend_cmd` in `iavf_handle_virtchnl_msg()`, then still runs `iavf_enable_irq0()` and returns.
4. If the closing thread wakes between 3 and the return, it goes on to `rte_intr_callback_unregister()`, gets `-EAGAIN`, and ignores it.
5. Close finishes with the callback still registered, or still running: one of the two signatures above.

On a normal host the window is a few microseconds and the closer wakes only once per millisecond, so the race is rare.

It becomes likely when `dpdk-intr` shares a CPU with the closing thread: the closer's 1 ms wake-up preempts the handler inside the window. That is the case in the exclusive partitions of `tests/tools/isolate/isolate.sh` (3 crashes in 27 runs), and in any container or `taskset` deployment with one or two CPUs for the non-worker threads.

## Why MTL carries the patch

MTL's timing tests run in an exclusive CPU partition, where the race is frequent enough to fail CI. MTL's stop → close → `rte_eal_cleanup()` order is already correct, and the callback and its argument are private to iavf, so MTL cannot unregister it safely itself.

The one MTL-side workaround was tried and rejected: close iavf ports from an EAL alarm, so the close runs on the interrupt thread and cannot overlap the callback. On large VFs (more than 16 queue pairs) it stalled close for 2 s and ended in a close error, and it does not cover iavf's auto-reset or init-error paths.

**Drop when** the DPDK release MTL pins ships the fix.

## Evidence

All on E830 VFs (ice PF), DPDK 26.07 with the MTL patch set.

- Window widened with the debug patch below: stock 10 of 10 runs SIGSEGV; patched 0 of 10.
- Repeated on 2026-09-25: stock 3 of 3 SIGSEGV, each with one unregister returning -11 (`-EAGAIN`).
- NoCtx in an `isolate.sh` exclusive partition, no widening: stock 3 crashes in 27 runs; patched 40 of 40 clean. Never seen in about 37 earlier campaigns without the partition.

## Reproduce without a NIC

### MTL unit tests

```sh
./build.sh unit
./build_unit/tests/unit/UnitTest --gtest_filter='EalIntrCallback.*:DpdkIavfPatch.*'
```

In `tests/unit/dev/iavf_intr_unregister_test.cpp`:

- `EalIntrCallback.UnregisterOfRunningCallbackKeepsIt` holds a callback in flight on the EAL interrupt thread and checks that `rte_intr_callback_unregister()` returns `-EAGAIN` and leaves it registered, which is the result `iavf_dev_close()` ignores.
- `EalIntrCallback.UnregisterSyncWaitsForRunningCallback` checks that `rte_intr_callback_unregister_sync()` returns only after the callback has returned, and removes the source.
- `DpdkIavfPatch.CloseWaitsForInFlightInterruptCallback` checks that the loaded iavf PMD imports `rte_intr_callback_unregister_sync` and not `rte_intr_callback_unregister`.

The two `EalIntrCallback` tests document the EAL behaviour the bug rests on and pass on any DPDK. `DpdkIavfPatch` is the one that fails on a DPDK built without 0009; rebuild with `script/build_dpdk.sh`, which applies `patches/dpdk/26.07/*.patch`.

### Standalone DPDK reproducer

This program replays iavf's teardown on eventfds: register a callback on the "vector 0" fd, fire it and hold it in flight, unregister it, close the fds as `pci_vfio_unmap_resource_primary()` does, and register one more source to force the epoll rebuild. It runs as a normal user, with no hugepages or devices, in about 1 s.

<!-- markdownlint-disable MD010 -->
```c
/* SPDX-License-Identifier: BSD-3-Clause
 * Replays iavf_dev_close() + rte_eal_cleanup() on eventfds, no NIC needed.
 * "bug" unregisters like iavf does today, "fixed" like patch 0009.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_interrupts.h>
#include <rte_stdatomic.h>
#include <rte_thread.h>

static RTE_ATOMIC(int) cb_running;
static RTE_ATOMIC(int) cb_release;

static void
vector0_handler(void *arg __rte_unused)
{
	rte_atomic_store_explicit(&cb_running, 1, rte_memory_order_release);
	while (!rte_atomic_load_explicit(&cb_release, rte_memory_order_acquire))
		rte_delay_us_sleep(100);
}

static void
other_handler(void *arg __rte_unused)
{
}

static uint32_t
release_later(void *arg __rte_unused)
{
	rte_delay_ms(100);
	rte_atomic_store_explicit(&cb_release, 1, rte_memory_order_release);
	return 0;
}

static struct rte_intr_handle *
eventfd_handle(int *fd)
{
	struct rte_intr_handle *h;

	*fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	h = rte_intr_instance_alloc(RTE_INTR_INSTANCE_F_PRIVATE);
	if (*fd < 0 || h == NULL ||
			rte_intr_type_set(h, RTE_INTR_HANDLE_VFIO_MSIX) ||
			rte_intr_fd_set(h, *fd))
		rte_exit(EXIT_FAILURE, "cannot create an eventfd interrupt handle\n");
	return h;
}

int
main(int argc, char **argv)
{
	char *eal_args[] = { argv[0], "--no-huge", "-m", "64", "--no-pci",
		"--in-memory", "-l", "0", "--log-level=lib.eal:error" };
	struct rte_intr_handle *vector0, *req, *next;
	int vector0_fd, req_fd, next_fd, fixed, ret;
	rte_thread_t releaser;
	uint64_t one = 1;

	if (argc != 2 || (strcmp(argv[1], "bug") && strcmp(argv[1], "fixed"))) {
		fprintf(stderr, "usage: %s bug|fixed\n", argv[0]);
		return 2;
	}
	fixed = !strcmp(argv[1], "fixed");
	if (rte_eal_init(RTE_DIM(eal_args), eal_args) < 0)
		rte_exit(EXIT_FAILURE, "rte_eal_init failed\n");

	/* 1. probe: the driver and the VFIO request notifier register. */
	vector0 = eventfd_handle(&vector0_fd);
	req = eventfd_handle(&req_fd);
	rte_intr_callback_register(vector0, vector0_handler, NULL);
	rte_intr_callback_register(req, other_handler, NULL);
	rte_delay_ms(50);

	/* 2. an adminq interrupt arrives while close is in progress. */
	if (write(vector0_fd, &one, sizeof(one)) != sizeof(one))
		rte_exit(EXIT_FAILURE, "cannot signal the eventfd\n");
	while (!rte_atomic_load_explicit(&cb_running, rte_memory_order_acquire))
		rte_delay_us_sleep(100);

	/* 3. close unregisters the callback that is still running. */
	if (fixed) {
		rte_thread_create_control(&releaser, "release", release_later, NULL);
		ret = rte_intr_callback_unregister_sync(vector0, vector0_handler, NULL);
		rte_thread_join(releaser, NULL);
	} else {
		ret = rte_intr_callback_unregister(vector0, vector0_handler, NULL);
		rte_atomic_store_explicit(&cb_release, 1, rte_memory_order_release);
	}
	printf("%s: unregister returned %d%s, vector 0 eventfd is fd %d\n",
		argv[1], ret, ret == -EAGAIN ? " (-EAGAIN, callback kept)" : "",
		vector0_fd);
	fflush(stdout);
	rte_delay_ms(50);

	/* 4. rte_eal_cleanup() -> pci_vfio_unmap_resource_primary(). */
	rte_intr_callback_unregister_sync(req, other_handler, NULL);
	close(req_fd);
	close(vector0_fd);
	rte_delay_ms(50);

	/* 5. the next change to the source list rebuilds the epoll set. */
	next = eventfd_handle(&next_fd);
	rte_intr_callback_register(next, other_handler, NULL);
	rte_delay_ms(200);
	rte_intr_callback_unregister_sync(next, other_handler, NULL);

	printf("teardown completed\n");
	return 0;
}
```
<!-- markdownlint-enable MD010 -->

```sh
export PKG_CONFIG_PATH=<dpdk-prefix>/lib/x86_64-linux-gnu/pkgconfig
cc -O2 -Wall -o intr_unregister_race intr_unregister_race.c \
   $(pkg-config --cflags --libs libdpdk) \
   -Wl,--disable-new-dtags -Wl,-rpath,$(pkg-config --variable=libdir libdpdk)
./intr_unregister_race bug     # exit 134
./intr_unregister_race fixed   # exit 0
```

Output with DPDK 26.07:

```text
bug: unregister returned -11 (-EAGAIN, callback kept), vector 0 eventfd is fd 10
EAL: PANIC in eal_intr_thread_main():
Error adding fd 10 epoll_ctl, Bad file descriptor
fixed: unregister returned 1, vector 0 eventfd is fd 10
teardown completed
```

## Reproduce on hardware

Use iavf VFs on an ice PF: iavf registers the callback only when the PF grants `VIRTCHNL_VF_OFFLOAD_WB_ON_ITR`, and ice does. Any DPDK application that closes them with `rte_eth_dev_close()` and then calls `rte_eal_cleanup()` will do, for example testpmd.

To hit the race on almost every close, build `librte_net_iavf.so` with this debug-only patch. It applies with or without 0009 and sleeps at the end of the handler, after the pending command is cleared:

<!-- markdownlint-disable MD010 -->
```diff
diff --git a/drivers/net/intel/iavf/iavf_ethdev.c b/drivers/net/intel/iavf/iavf_ethdev.c
index f7c34df..f01cdf6 100644
--- a/drivers/net/intel/iavf/iavf_ethdev.c
+++ b/drivers/net/intel/iavf/iavf_ethdev.c
@@ -6,6 +6,7 @@
 #include <sys/queue.h>
 #include <stdalign.h>
 #include <stdio.h>
+#include <stdlib.h>
 #include <errno.h>
 #include <stdint.h>
 #include <string.h>
@@ -2802,6 +2803,10 @@ iavf_dev_interrupt_handler(void *param)
 
 	iavf_handle_virtchnl_msg(dev);
 
+	/* DEBUG ONLY: hold the callback in flight after the pending command is cleared. */
+	if (getenv("IAVF_DBG_IRQ_DELAY_US"))
+		rte_delay_us_sleep(atoi(getenv("IAVF_DBG_IRQ_DELAY_US")));
+
 	iavf_enable_irq0(hw);
 }
 
```
<!-- markdownlint-enable MD010 -->

```sh
IAVF_DBG_IRQ_DELAY_US=5000 dpdk-testpmd -a <vf0> -a <vf1> -- -i
testpmd> port stop all
testpmd> port close all
testpmd> quit
```

- **Without 0009:** the process segfaults during close, or panics at exit with `Error adding fd N epoll_ctl, Bad file descriptor`, on almost every run.
- **With 0009:** close waits about 5 ms for the callback, and the process exits cleanly.
- **Without the debug patch:** confine all non-worker threads to one CPU, for example with `tests/tools/isolate/isolate.sh`; expect about 1 failure in 9 runs.

## Upstream status and ticket

- DPDK main (checked 2026-09-25, 4f795ddd6a) still has the non-sync call in `drivers/net/intel/iavf/iavf_ethdev.c` at lines 3200 (init error path) and 3295 (`iavf_dev_close()`). `lib/eal/linux/eal_interrupts.c` and `drivers/bus/pci/linux/pci_vfio.c` are unchanged between v26.07 and main.
- Patchwork has no pending fix (searched "iavf unregister", "iavf interrupt", "iavf close", "unregister_sync").
- The `.patch` next to this file is formatted for `dev@dpdk.org`, with `Fixes: 22b123a36d07 ("net/avf: initialize PMD")`, `Fixes: e35a5737a746 ("net/iavf: unregister intr handler before FD close")` and `Cc: stable@dpdk.org`. It is against v26.07 and applies to main at an offset (hunks at 3197 and 3292); rebase it before sending.
- Bugzilla ticket or cover mail: the `.patch` commit message as the description, plus the [Root cause](#root-cause) steps, the [Evidence](#evidence) and the [standalone reproducer](#standalone-dpdk-reproducer).
