# perf_testing

`perf_testing` measures the scheduler cost of the st20p RX payload paths. It starts N
identical st20p RX sessions on one port, and with `--tx` also N TX sessions on a second
port, so one host can send traffic to itself. With `--tx_only` it starts only N TX
sessions, to feed the RX sessions of a second process. At each interval it reads
`mtl_sch_get_stats()` for every scheduler slot and `st20p_rx_get_session_stats()` /
`st20p_tx_get_session_stats()` for every session, and prints one record set. Run it once
per RX path (CPU copy, `--dma`, `--hdr_split`) and compare the `work_*` columns of the RX
video tasklet `rvs_pkt_rx` and the `rxcost` record.

## What it measures

The library measures each tasklet handler call and each pass over all tasklets of a
scheduler. For each tasklet it keeps two distributions: `time` holds every call, `work`
holds only the calls that returned a value other than `MTL_TASKLET_ALL_DONE`.
`rvs_pkt_rx` returns the sum over its sessions, and a session is not done when
`mt_rxq_burst()` got packets or, with `--dma`, while DMA copies are in flight. Only the
calls that got packets copy the payload, submit the DMA, or do the header split
bookkeeping. The scheduler calls a polling tasklet millions of times per second, and
almost all calls are idle, so `busy_pct` and `avg_ns` of all calls barely change with the
load and cannot compare the RX paths. Compare `work_avg_ns`, `work_busy_pct` and
`ns_per_pkt` instead. The counters move only while the measurement is enabled:

- `--time_measure` sets `MTL_FLAG_TASKLET_TIME_MEASURE` at `mtl_init()`.
- Without the flag, attach the USDT probe `sys:tasklet_time_measure` from
  `lib/src/mt_usdt_provider.d` to the running process. The counters then move while the
  probe stays attached:

  ```bash
  sudo bpftrace -e 'usdt:/path/to/libmtl.so:sys:tasklet_time_measure { }'
  ```

  Use the `libmtl.so` the tool loaded
  (`ldd tests/tools/perf_testing/build/perf_testing | grep mtl`). Without the
  flag and without the probe the tool still runs and reports `time_measure` as `0`
  (`false` in JSON) with all time counters at zero.

The API counters are cumulative. The tool keeps the previous sample per scheduler and
tasklet and prints the difference per interval. It takes the first sample after the
sessions start, so the first interval does not hold the session create time.
`mtl_sch_get_stats()` reports at most `MTL_SCH_MAX_TASKLETS` (64) tasklets per
scheduler; `nb_tasklets_total` tells when a scheduler has more. `min_ns` and `max_ns`
have no per-interval form in the API, so they are the values since `mtl_init()` or the
last reset. Pass `--reset_each_interval` to call `mtl_sch_reset_stats()` before the
first interval and after each sample; then `min_ns` and `max_ns` are per interval too.

## Build

The tool is its own meson project and is not part of `build.sh`. Build the library first,
then point `pkg-config` at its `mtl.pc`:

```bash
MTL_INSTALL_PREFIX=$PWD/.local_install ./build.sh
export PKG_CONFIG_PATH=$(dirname "$(find .local_install -name mtl.pc)")
meson setup tests/tools/perf_testing/build tests/tools/perf_testing
ninja -C tests/tools/perf_testing/build
```

With a system install of the library (`./build.sh` as non-root installs to
`/usr/local`) the `PKG_CONFIG_PATH` line is not needed.

## Run

All runs need the hugepages and the VFs from `doc/run.md`. The TX port and the RX port
must be two different VFs on the same link, or two ports with a cable between them.

The tool calls `mtl_start()` before it creates the sessions, because the RX port answers
the ARP request of a unicast TX session only after `mtl_start()`.

`--rx_ip` selects the addressing. A multicast group (`224.0.0.0/4`, the default) is the
destination of the TX sessions and the group the RX sessions join. A unicast address is
the sender IP the RX sessions filter on; with `--tx` the tool ignores it for the wiring and
runs each stream from `--t_sip` to `--p_sip`.

CPU copy, 4 sessions, loopback TX, 60 s:

```bash
sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:01.0 --p_sip 192.168.85.80 \
  --tx --t_port 0000:af:01.1 --t_sip 192.168.85.81 \
  --sessions 4 --time_measure --duration 60 --interval 5
```

DMA offload, same traffic, two DSA devices:

```bash
sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:01.0 --p_sip 192.168.85.80 \
  --tx --t_port 0000:af:01.1 --t_sip 192.168.85.81 \
  --sessions 4 --time_measure --duration 60 \
  --dma 0000:80:04.0 --dma 0000:80:04.1
```

Header split, JSON output:

```bash
sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:01.0 --p_sip 192.168.85.80 \
  --tx --t_port 0000:af:01.1 --t_sip 192.168.85.81 \
  --sessions 4 --time_measure --duration 60 --hdr_split --json
```

`--hdr_split` sets `ST20P_RX_FLAG_HDR_SPLIT` on each session and
`nb_rx_hdr_split_queues` to the session count. The RX session sizes its frame slots from
`--packing`, so the TX side must send the same packing. A session uses
(3 + 1) *packets per frame - 1 slots, as the tool keeps 3 frames. The default of 21600
slots holds 1080p; 4K BPM needs 4* 16457 - 1 = 65827, so set `--hdr_split_slots 65827`
or more. The port must support buffer
split (the ice PF does); when it does not, `st20p_rx_create()` fails and the tool exits
with status 1.

`--tx_dst_mac` sets `ST20P_TX_FLAG_USER_P_MAC` and the destination MAC, so a unicast TX
session sends at once and does not wait for ARP. A switch that does not flood unknown
unicast then drops the stream until it learns the MAC of the RX port, so the RX port has
to send a frame first; the ARP exchange does that when `--tx_dst_mac` is not set.

`--duration 0` runs until SIGINT. The tool stops the sessions, frees them, and calls
`mtl_stop()` and `mtl_uninit()` on SIGINT and at the end of the duration.

### Sessions per core

`--sch_quota` sets `mtl_init_params.data_quota_mbs_per_sch`, the data rate in Mbit/s
that one scheduler takes before the library starts a new one. A quota above the sum of
all sessions puts all RX sessions on one scheduler. Increase `--sessions` until that
core cannot receive all frames. To find that point, divide the sum of the RX `frames` of
the RX process by the sum of the TX `frames` of the TX process. Both counts are
cumulative, so use the increase over the same interval. Do not use `busy_pct`: the loop
record of a polling scheduler is at about 99.9% at all loads. Do not use `work_busy_pct`
of `rvs_pkt_rx` or `rx_work_busy_pct` either: they stop at about 96% when almost every
call gets a packet. A hardware test reached that point at about 12 sessions on one core.
The work counters compare the RX cost only while the core is not saturated.

With `--tx` the TX sessions of the same process can share that scheduler, so send from a
second process with `--tx_only`. That process creates only TX sessions, on its
`--p_port`, and sends to `--rx_ip`, the `--p_sip` of the RX process. The RX process uses
the `--p_sip` of the TX process as its `--rx_ip`. Start the RX process first, because a
unicast TX session waits for the ARP reply of the RX port.

16 sessions of 1080p59 (about 2.6 Gbps each) on one scheduler:

```bash
sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:01.0 --p_sip 192.168.85.80 --rx_ip 192.168.85.81 \
  --sessions 16 --sch_quota 100000 --lcores 4,5 --time_measure --duration 70

sudo tests/tools/perf_testing/build/perf_testing \
  --p_port 0000:af:01.1 --p_sip 192.168.85.81 --rx_ip 192.168.85.80 \
  --tx_only --sessions 16 --lcores 8,9,10,11 --duration 60
```

Give the two processes different `--lcores`. MTL starts DPDK with `--in-memory`, so the
two processes share no DPDK files.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--p_port <pci>` | required | RX port; the TX port with `--tx_only` |
| `--p_sip <ip>` | required | IP of `--p_port` |
| `--rx_ip <ip>` | `239.168.85.20` | Multicast group, or the sender IP of a unicast stream; the destination IP with `--tx_only` |
| `--udp_port <n>` | `20000` | UDP port of session 0; session i uses `n + i` |
| `--sessions <n>` | `1` | RX sessions, and TX sessions with `--tx`; only TX sessions with `--tx_only` |
| `--width`, `--height` | `1920`, `1080` | Resolution |
| `--fps` | `p59` | `p59`, `p50`, `p29`, `p25`, `p60`, `p30` |
| `--fmt` | `YUV_422_10bit` | Transport format; the output format is the same, so no conversion runs |
| `--tx` | off | Also create TX sessions on `--t_port` / `--t_sip` |
| `--tx_only` | off | Only TX sessions, on `--p_port`, sent to `--rx_ip`; excludes `--tx`, `--hdr_split` and `--dma` |
| `--packing` | `bpm` | TX and RX `transport_packing`: `bpm`, `gpm`, `gpm_sl` |
| `--tx_dst_mac <mac>` | none | TX destination MAC, sets `ST20P_TX_FLAG_USER_P_MAC` and skips ARP |
| `--sch_quota <mbs>` | `0` | `mtl_init_params.data_quota_mbs_per_sch`; `0` keeps the library default |
| `--hdr_split` | off | Header split RX queues |
| `--hdr_split_slots <n>` | `0` | `mtl_init_params.hdr_split_slots`, the payload mbufs of each header split queue; `0` keeps the library default, 21600 |
| `--dma <pci>` | none | DMA device, repeat for more; sets `ST20P_RX_FLAG_DMA_OFFLOAD` |
| `--time_measure` | off | Set `MTL_FLAG_TASKLET_TIME_MEASURE` |
| `--interval <s>` | `5` | Sample interval |
| `--duration <s>` | `30` | Runtime, `0` runs until SIGINT |
| `--json` | off | One JSON object per interval on one line |
| `--reset_each_interval` | off | Call `mtl_sch_reset_stats()` before the first interval and after each sample |
| `--lcores <list>` | none | `mtl_init_params.lcores` |
| `--log_level` | `warning` | `debug`, `info`, `notice`, `warning`, `error` |

## Output

### CSV

Three header lines start with `#`, two with `--tx_only`. Each later line is one record;
the second column tells the record kind.

Scheduler records (`kind` is `loop` or `tasklet`):

```text
ts,kind,sch_idx,sch_name,lcore,time_measure,tasklet_idx,tasklet_name,cnt,avg_ns,min_ns,max_ns,busy_pct,avg_ns_per_loop,work_cnt,work_avg_ns,work_busy_pct
```

| Column | Meaning |
| --- | --- |
| `ts` | Wall clock of the sample, seconds since the epoch |
| `sch_idx`, `sch_name` | Scheduler slot and name |
| `lcore` | Lcore of the scheduler, `-1` in thread mode |
| `time_measure` | `1` while the counters move |
| `tasklet_idx`, `tasklet_name` | Tasklet slot and name; `-1` and `loop` for the loop record |
| `cnt` | Handler calls (or loop passes) in the interval |
| `avg_ns` | `sum_ns / cnt` over the interval |
| `min_ns`, `max_ns` | Since `mtl_init()` or the last reset |
| `busy_pct` | `100 * sum_ns / interval_ns` |
| `avg_ns_per_loop` | The scheduler's own loop average, always available |
| `work_cnt` | Handler calls in the interval that returned a value other than `MTL_TASKLET_ALL_DONE`; `0` for the loop record |
| `work_avg_ns` | `work_sum_ns / work_cnt` over the interval |
| `work_busy_pct` | `100 * work_sum_ns / interval_ns` |

Session records (`kind` is `session`), cumulative since the session start:

```text
ts,kind,dir,idx,frames,pkts,lost,unrecovered
```

`frames` is the count of frames the tool got from `st20p_rx_get_frame()` or put with
`st20p_tx_put_frame()`. The tool never reads or writes frame data; the TX frames carry
the zero fill of the library. `pkts`, `lost` and `unrecovered` come from the session
stats; a TX record has `lost` and `unrecovered` at `0`. With `--tx_only` each session
has only a TX record.

One RX cost record (`kind` is `rxcost`) per interval, after the session records. With
`--tx_only` the tool prints no RX cost record:

```text
ts,kind,scope,rx_pkts,rx_work_ns,ns_per_pkt,rx_work_busy_pct
```

| Column | Meaning |
| --- | --- |
| `scope` | Always `all`: the sum over all schedulers and all RX sessions |
| `rx_pkts` | RX packets of all sessions in the interval, from `pkts` of the session records |
| `rx_work_ns` | Work time (`work_sum_ns`) of all `rvs_pkt_rx` tasklets in the interval |
| `ns_per_pkt` | `rx_work_ns / rx_pkts`, `0` when `rx_pkts` is `0` |
| `rx_work_busy_pct` | `100 * rx_work_ns / interval_ns`, the sum of `work_busy_pct` of the `rvs_pkt_rx` tasklets |

With `--dma`, `work` means "got packets, or DMA copies in flight". The calls that only
poll for DMA completions add to `rx_work_ns` and bring no packets, so `ns_per_pkt` of a
DMA run includes that poll time.

`rx_work_ns` holds only the tasklets named `rvs_pkt_rx` that `mtl_sch_get_stats()`
reports. It does not hold the work of a separate packet lcore. A session uses that lcore
with `ST20_RX_FLAG_USE_MULTI_THREADS`, or above 40 Gbps when it has no DMA device, but
never with `--hdr_split`. It also does not hold the work of a tasklet past
`MTL_SCH_MAX_TASKLETS` on its scheduler.

### JSON

One object per interval. Each tasklet has a `time` and a `work` object with the same
fields; `rx_cost` holds the fields of the `rxcost` CSV record and is absent with
`--tx_only`:

```json
{"ts":1760000000.123,"interval_ns":5000000000,
 "sch":[{"idx":0,"name":"sch_0","lcore":4,"time_measure":true,"avg_ns_per_loop":812,
         "loop":{"cnt":6150000,"avg_ns":812,"min_ns":90,"max_ns":45120,"busy_pct":99.9},
         "tasklets":[{"idx":0,"name":"rvs_pkt_rx","time":{"cnt":6150000,"avg_ns":640,"min_ns":40,"max_ns":44000,"busy_pct":78.7},
                      "work":{"cnt":1200000,"avg_ns":2100,"min_ns":300,"max_ns":44000,"busy_pct":50.4}}]}],
 "sessions":[{"dir":"rx","idx":0,"frames":2996,"pkts":12500000,"lost":0,"unrecovered":0}],
 "rx_cost":{"pkts":2580000,"work_ns":2520000000,"ns_per_pkt":976.74,"work_busy_pct":50.40}}
```
