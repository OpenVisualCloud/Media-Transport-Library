# 0017 net/iavf: fix event use after release on close

One `iavf-event` thread handles the PF events of all iavf ports of the process. `iavf_dev_close()` does not remove the events of its port from that thread, and does not wait for the event that the thread handles. `rte_eth_dev_close()` releases the port right after `iavf_dev_close()` returns, so the thread can handle an event of a released port.

The patch makes the thread handle one event at a time and record its port. At the end of `iavf_dev_close()`, the new `iavf_dev_event_discard()` removes the pending events of the port and waits until the thread does not hold an event of the port.

## What breaks

The process gets SIGSEGV in the event thread at exit, right after MTL logs `dev_close_port(0), succ`. Seen on 2026-11-26 on VM 140: an FFmpeg MTL RX process on an E830 VF, host ice 2.7.6 PF, DPDK `26.07.93_mtl_0`:

```text
dpdk-iavf-event[25997]: segfault at 60 ip 0000754ca1383b92 sp 0000754c99ffaba0 error 4 in librte_net_iavf.so.26.2[754ca1327000+6b000]
```

The text segment has LOAD vaddr 0x7000, so the file address is 0x63b92. It is in `iavf_dev_event_handle()`: `mov 0x48(%rdi),%rax; mov 0x60(%rax),%rax`, the read of `pos->dev->data->dev_private` with `data == NULL`.

## Root cause

Line numbers are of DPDK v26.07.

1. `iavf_dev_event_handler_init()` (`iavf_vchnl.c:128`) starts one `iavf-event` thread for the process, and counts the ports in `handler->ndev`.
2. `iavf_dev_event_post()` (`iavf_vchnl.c:100`) adds an event to `handler->pending`. The interrupt handler, the alarm handler, the watchdog and the polled virtchnl reads post the link change (LSC) and reset events of the PF.
3. `iavf_dev_event_handle()` (`iavf_vchnl.c:63`) moves the whole pending list to a local list under `handler->lock` (`TAILQ_CONCAT`, line 76). Then it handles each event without the lock: it reads `pos->dev->data->dev_private` and calls `iavf_handle_hw_reset()` or `rte_eth_dev_callback_process()`.
4. `rte_eth_dev_close()` (`lib/ethdev/rte_ethdev.c:1989`) calls `iavf_dev_close()` (`iavf_ethdev.c:3162`), then `rte_eth_dev_release_port()`, which sets `dev->data = NULL` (`ethdev_driver.c:298`).
5. `iavf_dev_event_handler_fini()` runs only from `iavf_dev_uninit()` (`iavf_ethdev.c:3287`), at bus remove in `rte_eal_cleanup()`, and stops the thread only when the last port goes.
6. So an event of the closing port that is pending or in hand at step 4 is handled after the release. The PF sends such events while the VF stops and closes, for example the link change and the reset events.

The window is between the last event post of the port and the release. The thread is idle most of the time, so the fault is rare, but MTL apps close all ports at each exit.

## The fix

- The thread takes one event at a time from `handler->pending`, under the lock, and writes its port to `handler->handling_dev`. It clears the field under the lock after the event.
- `iavf_dev_event_discard(dev)` runs at the end of `iavf_dev_close()`, after the interrupt callback, the alarm and the watchdog are stopped, so nothing posts an event of the port after it. It removes and frees each pending event of the port, then waits while `handling_dev == dev`.
- The wait polls every 1 ms under the existing mutex, as `iavf_wait_for_msg()` does. A condition variable would wake at once, but the pthread shim of DPDK on Windows (`lib/eal/windows/include/pthread.h`) has no `pthread_cond_t`, and iavf builds on Windows. Close is a slow path: 1 ms is small against the virtchnl commands of close.
- The event thread closes a port itself: `iavf_handle_hw_reset()` (auto-reset), or an application callback that calls `rte_eth_dev_reset()`. Then the event in hand is its own, so `iavf_dev_event_discard()` compares `rte_thread_self()` with `handler->tid` and skips the wait. It still drops the pending events of the port, which describe the port before the reset.
- With `handler->ndev == 0` there is no thread, and the mutex can be destroyed, so the function returns. That is the case for a close after an `iavf_dev_reset()` whose `iavf_dev_init()` failed.

### Paths that the patch must not break

- **Auto-reset** (`in_reset_recovery`): the event thread runs `iavf_handle_hw_reset()` → `iavf_dev_reset()` → `iavf_dev_uninit()` → `iavf_dev_close()`. The discard runs on the event thread, so it does not wait. `iavf_dev_uninit()` still skips `iavf_dev_event_handler_fini()`, and `iavf_dev_init()` still skips `iavf_dev_event_handler_init()`, so `ndev` does not change.
- **Init error path**: `iavf_dev_init()` does not call `iavf_dev_close()` on an error, so the discard does not run there and the path does not change.
- **fini**: `iavf_dev_event_handler_fini()` does not change. It runs after `iavf_dev_close()`, so the last port is discarded before the thread is cancelled. The thread holds the mutex only for list operations, which have no cancellation point.
- **A close on another thread** now waits for the event callback of its port to return. An application LSC or reset callback that waits for the thread that closes the port deadlocks. `rte_intr_callback_unregister_sync()` (patch 0009) has the same contract.

### What the patch does not cover

- The init error path releases the port with no close. An event that the polled virtchnl read posted during `iavf_init_vf()` can stay pending there. The upstream fix "net/iavf: fix event handler leak on init failure" changes these error paths; a discard there belongs on top of it.
- An application reset from an event callback of the last iavf port runs `iavf_dev_event_handler_fini()` on the event thread, which cancels and joins itself. That fault is older than this patch and not changed by it.

## Why MTL carries the patch

Each MTL app closes its iavf ports in `mtl_uninit()` and runs `rte_eal_cleanup()` after it. The crash comes after the work is done, but it kills the process with SIGSEGV, so a test or a pipeline that checks the exit status fails. The event thread and its list are private to iavf, so MTL cannot drain them. A delay between close and `rte_eal_cleanup()` does not help: the release is in `rte_eth_dev_close()`.

**Drop when** the DPDK release MTL pins ships the fix.

## Evidence

- Unit fingerprint: `DpdkIavfPatch.CloseWaitsForInFlightEventOfThePort` fails on the series without 0017 and passes with it (see below).
- The series applies to a clean v26.07 with `git am --3way` and with `patch -p1` (as `script/build_dpdk.sh` does), and the iavf driver builds with `-Dwerror=true`. `devtools/checkpatches.sh` and `devtools/check-git-log.sh` pass.
- Hardware: to do on VM 140 with the recipe below.

## Reproduce without a NIC

No unit test can show the fault: the event functions are `static` or hidden in `librte_net_iavf.so`, and the event thread starts only when an iavf port probes.

```sh
./build.sh unit
./build_unit/tests/unit/UnitTest --gtest_filter='DpdkIavfPatch.*'
```

`DpdkIavfPatch.CloseWaitsForInFlightEventOfThePort` in `tests/unit/dev/iavf_intr_unregister_test.cpp` checks that the loaded iavf PMD imports `rte_thread_self` and `rte_thread_equal`. In the iavf PMD of 26.07 only the event thread check of `iavf_dev_event_discard()` uses them. It fails on a DPDK built without 0017; rebuild with `script/build_dpdk.sh`.

## Reproduce on hardware

Use iavf VFs on an ice PF, for example the E830 VFs of VM 140. Make two DPDK prefixes in a directory that is not world-writable (not `/tmp`): one with each patch but 0017, one with all of them (0001-0017).

### Debug widening

This script (`iavf_event_debug.sh`), for a debug build only, applies to both source trees. It adds two switches:

- `IAVF_DBG_EVENT_DELAY_US`: the event thread sleeps after it takes an event and before it reads the port, so a release falls into the window.
- `IAVF_DBG_EVENT_AT_CLOSE`: `iavf_dev_close()` posts one LSC event of its port at its end, before the discard of 0017. It replaces the PF event, which comes at a random time.

```sh
#!/bin/sh
# DEBUG ONLY: widen the iavf event race. Run in a DPDK 26.07 source tree.
set -e
v=drivers/net/intel/iavf/iavf_vchnl.c
e=drivers/net/intel/iavf/iavf_ethdev.c
sed -i '0,/^#include <stdio.h>$/s//#include <stdio.h>\n#include <stdlib.h>/' "$v" "$e"
sed -i '/^\t\t\tstruct iavf_adapter \*adapter = pos->dev->data->dev_private;$/i\
\t\t\tif (getenv("IAVF_DBG_EVENT_DELAY_US"))\
\t\t\t\trte_delay_us_sleep(atoi(getenv("IAVF_DBG_EVENT_DELAY_US")));' "$v"
sed -i '/^out:$/a\
\tif (getenv("IAVF_DBG_EVENT_AT_CLOSE"))\
\t\tiavf_dev_event_post(dev, RTE_ETH_EVENT_INTR_LSC, NULL, 0);' "$e"
grep -c IAVF_DBG_ "$v" "$e"
```

```sh
mtl=<MTL tree>; w=$HOME/mtl/work/iavf-event-hw
mkdir -p "$w" && cd "$w"
for t in stock patched; do
  rm -rf src-$t && mkdir src-$t
  git -C <dpdk clone> archive v26.07 | tar -x -C src-$t
  (cd src-$t &&
   for f in "$mtl"/patches/dpdk/26.07/00*.patch; do
     [ $t = stock ] && case $f in *0017-*) continue ;; esac
     patch -p1 -s -i "$f"
   done &&
   sh ../iavf_event_debug.sh &&
   meson setup ../build-$t --prefix="$w/prefix-$t" -Dmax_lcores=128 \
     -Denable_drivers=bus/pci,bus/vdev,mempool/ring,net/intel/iavf \
     -Ddisable_apps='*' -Dtests=false -Denable_docs=false &&
   ninja -C ../build-$t install)
done
```

### Close app

The app (`iavf_close_event.c`) opens each port, waits `UP_MS` (default 1000), stops and closes each port, then waits `AFTER_MS` (default 200) before `rte_eal_cleanup()`. During that wait the event thread is alive and the ports are released.

<!-- markdownlint-disable MD010 -->
```c
/* SPDX-License-Identifier: BSD-3-Clause
 * Opens each port, closes it, and stays alive while the iavf-event thread
 * handles what is left; exits 0 when no thread touched a released port.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

static int
open_port(uint16_t port, struct rte_mempool *mp)
{
	struct rte_eth_conf conf = { 0 };
	int ret;

	ret = rte_eth_dev_configure(port, 1, 1, &conf);
	if (ret == 0)
		ret = rte_eth_rx_queue_setup(port, 0, 512, rte_eth_dev_socket_id(port), NULL, mp);
	if (ret == 0)
		ret = rte_eth_tx_queue_setup(port, 0, 512, rte_eth_dev_socket_id(port), NULL);
	if (ret == 0)
		ret = rte_eth_dev_start(port);
	return ret;
}

int
main(int argc, char **argv)
{
	unsigned int up_ms = getenv("UP_MS") ? atoi(getenv("UP_MS")) : 1000;
	unsigned int after_ms = getenv("AFTER_MS") ? atoi(getenv("AFTER_MS")) : 200;
	struct rte_mempool *mp;
	uint16_t port;
	int ret;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eal_init failed\n");
	if (rte_eth_dev_count_avail() == 0)
		rte_exit(EXIT_FAILURE, "no ports\n");
	mp = rte_pktmbuf_pool_create("mp", 4096, 256, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());
	if (mp == NULL)
		rte_exit(EXIT_FAILURE, "cannot create the mbuf pool\n");

	RTE_ETH_FOREACH_DEV(port) {
		ret = open_port(port, mp);
		if (ret)
			rte_exit(EXIT_FAILURE, "port %u: open failed %d\n", port, ret);
	}
	rte_delay_us_sleep(up_ms * 1000);

	RTE_ETH_FOREACH_DEV(port) {
		rte_eth_dev_stop(port);
		ret = rte_eth_dev_close(port);
		printf("port %u closed: %d\n", port, ret);
		fflush(stdout);
	}

	/* The event thread runs until rte_eal_cleanup() cancels it. */
	rte_delay_us_sleep(after_ms * 1000);
	rte_mempool_free(mp);
	rte_eal_cleanup();
	printf("clean exit\n");
	return 0;
}
```
<!-- markdownlint-enable MD010 -->

```sh
for t in stock patched; do
  export PKG_CONFIG_PATH=$w/prefix-$t/lib/x86_64-linux-gnu/pkgconfig
  cc -O2 -Wall -o iavf_close_event_$t iavf_close_event.c \
     $(pkg-config --cflags --libs libdpdk) \
     -Wl,--disable-new-dtags -Wl,-rpath,$(pkg-config --variable=libdir libdpdk)
done
```

### Run

Bind two VFs to `vfio-pci` and stop each other DPDK process on them first.

```sh
run() { # run <stock|patched> <count>
  for i in $(seq "$2"); do
    sudo IAVF_DBG_EVENT_AT_CLOSE=1 IAVF_DBG_EVENT_DELAY_US=20000 \
      ./iavf_close_event_$1 -l 1 -a <vf0> -a <vf1> --file-prefix=iavfev \
      --log-level=pmd.net.iavf.*:notice >/dev/null 2>&1
    echo "exit $?"
  done | sort | uniq -c
}
run stock 20
run patched 20
sudo dmesg | grep -c 'dpdk-iavf-event.*segfault'
```

Expected (from the code, to be measured):

| Build | Widening | Result |
| --- | --- | --- |
| stock (0001-0014) | `AT_CLOSE` + `DELAY_US=20000` | 20 of 20 `exit 139`; each run adds one `dpdk-iavf-event[...]: segfault at 60 ... in librte_net_iavf.so` line to the kernel log |
| patched (0001-0017) | `AT_CLOSE` + `DELAY_US=20000` | 20 of 20 `exit 0` and `clean exit`; a close takes up to 20 ms longer while it waits for the event in hand |
| stock | `DELAY_US=200000`, no `AT_CLOSE`, `UP_MS=0` | a crash only when the PF sends an event of a port in the 200 ms before its close; the rate depends on the PF |
| patched | the same | 0 crashes |

With `IAVF_DBG_EVENT_AT_CLOSE=1` and no delay, the stock build also crashes, but only when the thread is slower than the release.

### With MTL

The fault needs only a close of an iavf port with an event of that port pending. Run RxTxApp (or the FFmpeg MTL plugin) on VFs, built against the prefix (`PKG_CONFIG_PATH`), with `IAVF_DBG_EVENT_AT_CLOSE=1 IAVF_DBG_EVENT_DELAY_US=20000` in its environment and a short `--test_time`, 20 times.

Expect the counts of the app above: SIGSEGV after `dev_close_port(0), succ` on the stock build, exit 0 on the patched one.

Both prefixes report `26.07.96_mtl_`, because 0002 sets the version. Tell them apart by the PMD: only the patched one imports `rte_thread_equal`.

```sh
nm -D --undefined-only <prefix>/lib/x86_64-linux-gnu/dpdk/pmds-26.2/librte_net_iavf.so.26.2 | grep rte_thread_equal
```

## Upstream status and ticket

- DPDK v26.07 and `origin/main` of the local clone (2026-08-24) have the same event handler: `git diff v26.07 origin/main -- drivers/net/intel/iavf/iavf_vchnl.c` is empty.
- Two upstream fixes are near, and neither covers this fault:
  - "net/iavf: fix event handler refcount leak on HW reset" (e72adb67cc, 2026-06-08, in v26.07): the auto-reset path no longer takes a second `ndev` reference. It changes when the thread stops, not which events it handles.
  - "net/iavf: fix event handler leak on init failure" (accepted 2026-11-06): the init error paths release the `ndev` reference. It does not remove events, and it does not touch `iavf_dev_close()`.
- The `.patch` next to this file is formatted for `dev@dpdk.org`, with `Fixes: cb5c1b91f76f ("net/iavf: add thread for event callbacks")` and `Cc: stable@dpdk.org`. It is against v26.07 with MTL patches 0001-0014; it touches only `iavf.h`, `iavf_vchnl.c` and the end of `iavf_dev_close()`. Rebase it on main before sending, after the init failure fix.
- Bugzilla ticket or cover mail: the `.patch` commit message as the description, plus the [Root cause](#root-cause) steps and the [hardware recipe](#reproduce-on-hardware).
