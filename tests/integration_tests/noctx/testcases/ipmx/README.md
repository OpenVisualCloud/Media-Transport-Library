# NoCtx IPMX cases

Built only when the installed MTL has IPMX (`mtl.pc` variable `ipmx=true`, from
`-Denable_ipmx=true`). Requirements, SKIP/FAIL policy and running are as for the
strict cases in [../../README.md](../../README.md). `run.sh` runs these cases under
`isolate.sh` (`NOCTX_ISOLATE_CASES`).

Each case runs one IPMX st20p session (TX `ST20P_TX_FLAG_IPMX`, RX
`ST20P_RX_FLAG_IPMX | ST20P_RX_FLAG_TIMING_PARSER_META`) with default RL pacing for
20 s. After stop it reads `ipmx_st20p_rx_get_tp_stat()` on port P: the library's
TR-10-1 section 8.1 CMAX / VRX model run on NIC RX timestamps. The KahawaiTest
`Ipmx_st20p.*_tp_*` cases check only the plumbing; the model itself is pinned in
`tests/unit/ipmx/`.

| Test | Proves | Oracle | Tolerance |
|---|---|---|---|
| `st20p_ipmx_tp_1080p60` | RL pacing of 1080p60 YUV 4:2:2 10-bit keeps every IPMX frame within CMAX and VRX | `frames_failed == 0`; `cmax` 16, `vrx_full` 32; the first packet of every frame NIC timed (`RxPhcClock`) | ≥ 600 measured frames and ≥ 50 % of the frames seen |
| `st20p_ipmx_tp_2160p60` | The same at 2160p60 YUV 4:2:2 10-bit | As above, `cmax` 45, `vrx_full` 90 | As above |
| `st20p_ipmx_tp_rgb10_2160p60` | The same at 2160p60 RGB 4:4:4 10-bit, the heaviest D6 rate (~16 Gb/s) | As above, `cmax` 68, `vrx_full` 136 | As above |

Measured on E835 VFs (TX on PF1, RX on PF0), run as root:

| Test | Compliant | Unmeasured | CINST max / CMAX | VRX min..max / VRXFULL |
|---|---|---|---|---|
| `st20p_ipmx_tp_1080p60` | 1199 | 0 | 3 / 16 | 15..18 / 32 |
| `st20p_ipmx_tp_2160p60` | 1197 | 1 | 5 / 45 | 42..47 / 90 |
| `st20p_ipmx_tp_rgb10_2160p60` | 1198 | 1 | 6 / 68 | 49..71 / 136 |

The PHC check needs read access to `/dev/ptpN`, which is root-only by default. On
E835 only PF0 exposes `device/ptp`, so RX must be a VF of PF0.

## Create/free cycles

`st20p_ipmx_create_free_cycles` and its baseline `st20p_noipmx_create_free_cycles` run
30 cycles of one 1080p60 RGB 4:4:4 8-bit st20p pair (create, `mtl_start()`, 2 s,
`mtl_stop()`, free) on an instance without `MTL_FLAG_RANDOM_SRC_PORT`, so every cycle
reuses one UDP tuple and the CNI UDP stream detect list, one entry per tuple until
`mtl_uninit()`, stops growing after cycle 1. No strict topology or PHC is needed; with
fewer than 2 ports the case errors (`std::runtime_error`).

| Test | Proves | Oracle |
|---|---|---|
| `st20p_ipmx_create_free_cycles` | An IPMX TX/RX pair, Sender Report state and pool included, gives back every DPDK heap allocation it takes | Every cycle receives >= 60 frames (half of 2 s at 60 fps) whose TX stamps at the first and last word match; after the last cycle the DPDK heap live allocations and bytes are <= after cycle 0 |
| `st20p_noipmx_create_free_cycles` | The same without the IPMX flags | As above |

Both log the heap after every cycle (`NoCtx ipmx cycles(N)`). `IPMX_TEST_HEAP_DUMP=1`
adds `rte_memzone_dump()` and `rte_malloc_dump_heaps()` after the first and the last
cycle.

