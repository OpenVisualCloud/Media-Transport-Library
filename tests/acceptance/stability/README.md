# Stability workload

The stability engine runs a long, mixed MTL workload on one host and judges it
while it runs. The `stability` workflow (`.github/workflows/stability.yml`) runs
it through the pytest case `tests/single/stability/test_stability.py`. You can
also run it by hand on a lab host or a VM.

## What runs

| Worker | Tool | What it does |
|---|---|---|
| `rxtxapp-mix` | RxTxApp | Sends each session type of MTL from one process. |
| `rxtxapp-rx` | RxTxApp | Receives each session of `rxtxapp-mix`. Uses the internal converters. |
| `rxtxapp-extconv` | RxTxApp | Receives the 1080p59 streams again through the converter plugin `color_convert_sample`. |
| `rxtxapp-integrity` | RxTxApp | Sends and receives three st20p streams with a SHA-256 check of each frame. |
| `ffmpeg-tx` | FFmpeg | Converts an RGB test source with swscale and sends st20p. With `libsvtjpegxs`, it also encodes JPEG XS and sends it as st22. |
| `ffmpeg-rx` | FFmpeg | Receives the st20p stream of `ffmpeg-tx`, converts it with swscale and writes a `framemd5` file. |
| `gst-tx` | GStreamer | Sends st20p and st30p with `use-pts-for-pacing`. |

The sessions of `rxtxapp-mix`:

| Stream | API | User pacing | Note |
|---|---|---|---|
| `st20p-2160p59-int` | st20p | yes | Planar 10 bit input: the internal converter |
| `st20p-1080p59-int` | st20p | yes | Internal converter. `rxtxapp-extconv` receives it too. |
| `st20p-1080p59-v210` | st20p | yes | V210 input: the internal converter |
| `st20p-1080p59-exact` | st20p | exact | RFC 4175 input: no converter (derive) |
| `st20p-1080p50-int` | st20p | yes | 50 Hz in the same process as the 59.94 Hz sessions |
| `st20p-720p59-int` | st20p | yes | Internal converter |
| `st20-1080p59-frame` | st20 frame | yes | Frame API |
| `st20-1080p59-rtp` | st20 RTP | yes | RTP API |
| `st20-720p50-slice` | st20 slice | no | Slice API |
| `st22p-1080p59-jpegxs` | st22p | no | Encoder plugin (when `st22_codec` is set) |
| `st30p-pcm24-48k-1ms` | st30p | yes | |
| `st30p-pcm24-96k-0.125` | st30p | yes | |
| `st30-pcm16-48k-8ch` | st30 | no | |
| `st40p-p59` | st40p | yes | |
| `st40-p59` | st40 | yes | |
| `st41-p59` | st41 | no | EBU LIST cannot read ST 2110-41, so it gets no verdict. |

Each TX session has its own multicast group (`239.30.<worker>.<n>`). Thus, a
capture filter on the destination address holds exactly one stream.

`rxtxapp-integrity` (with a 4th TX and RX port) sends three st20p streams with
`--video_sha_check` and receives them in the same process. TX sends the SHA-256
of each frame in a user-meta packet, and RX compares it with the frame. That
packet has the RTP sequence number 0, and EBU LIST then reports `rtp_sequence`
and "general" packing for the stream. Thus these streams get no EBU LIST
verdict, and no other worker uses `--video_sha_check`.

## How the run is judged

While the workers run, the engine does these steps again and again:

1. It reads the new lines of each worker log. It records the fps of each MTL
   session of each stat dump, and each line of a known error class
   (`logscan.ERROR_CLASSES`).
2. It captures one stream (see [EBU LIST rotation](#ebu-list-rotation)) and
   gets the EBU LIST verdict. Then it goes to the next stream.
3. It writes `status.json` in the run directory.

The run passes when all these conditions are true:

- Each worker runs to the end and stops with exit status 0.
- Each RxTxApp session result is `OK`.
- No log has a crash, a SHA mismatch or a failed result.
- No session fps falls below 98 % of the median of that session after the
  first 30 s.
- No EBU LIST verdict is `fail` or `no-stream`.

`summary.json` gives each reason of a failed run. `report.html` and
`report.md` show the workers, the streams, the EBU LIST verdicts in time order,
the error lines and the fps of each session.

## EBU LIST rotation

For each stream that EBU LIST can read, one pass of the rotation:

1. Joins the multicast group on the capture netdev. The switch snoops IGMP.
2. Adds a tc ingress rule (preference 2111) that drops the group after the
   capture tap, so the IP stack does not get the stream.
3. Captures a fixed packet count with netsniff-ng: hardware RX timestamps and a
   nanosecond pcap. For video, it captures `video_frames` frames.
4. Reads the drop counters of the netdev. When the netdev dropped packets, the
   verdict is `capture-loss`. Then the capture has the fault, not the stream.
5. Uploads the pcap, waits for the report, and judges it with the checks of
   `mtl_engine.pcap_compliance` (resolution, rate, packing, narrow or wide) and
   each EBU LIST analysis of the stream.
6. Deletes the pcap from EBU LIST. The engine keeps a failed pcap on the host,
   up to `keep_failed_pcaps`. It deletes the other pcaps.

A video stream that is only "wide" fails, unless `ebu.allow_wide` is true. TSC
pacing in a VM can give "wide".

## CPU fence

The fence is the same as the fence of the performance suite, but for many
processes. `tests/tools/isolate/isolate.sh` puts one command into one isolated
partition. In an isolated partition no load balancing runs, so each thread
that is not pinned stays on the CPU where it starts. That is correct for one
RxTxApp, but not for an FFmpeg encoder or a GStreamer pipeline. Thus, the
engine uses three CPU sets (`fence.py`):

| Set | What runs there |
|---|---|
| `housekeeping` | Each systemd slice, the IRQs and the unbound workqueues |
| `app` | The threaded cgroup `mtl-stability/app`, with load balancing: each thread of a worker that is not an MTL scheduler |
| `lcores` | The threaded cgroup `mtl-stability/lc`, an isolated partition: each MTL scheduler thread |

Each worker starts in `mtl-stability`, where EAL pins its lcores. After 20 s,
the engine moves each thread to `lc` or `app` and makes `lc` an isolated
partition. It also sets `kernel.numa_balancing=0` and `vm.stat_interval=120`,
and turns off the idle states with an exit latency above 10 µs. It writes each
change to `/run/mtl-stability-fence.json` first. At the end of the run it
restores each change. After a crash, `python -m stability unfence` restores them.

## Run it by hand

```bash
cd tests/acceptance
cp stability/stability.yaml.example stability.yaml   # then edit the ports, CPUs, capture NIC
sudo -E venv/bin/python -m stability check -c stability.yaml
sudo -E venv/bin/python -m stability plan  -c stability.yaml -p full -d 15m   # print the plan
sudo -E venv/bin/python -m stability run   -c stability.yaml -p full -d 8h
sudo -E venv/bin/python -m stability unfence                                 # after a killed run
```

Run the engine as root: the fence, vfio, tc and netsniff-ng need root. During
the run, `<run_dir>/<time>-<profile>/status.json` shows the state.

## Host config

`stability.yaml.example` shows each key. The EBU LIST password is not in the
file. `ebu.password_file` names a file of `KEY=VALUE` lines with
`EBU_PASSWORD`. On a CI runner this is `/etc/mtl-ci/runner.env`, which also
holds `EBU_IP` and `EBU_USER`.

In CI, the case reads `/etc/mtl-ci/stability.yaml` (or `STABILITY_CONFIG`).
When the file is not there, the case makes 6 VFs on the test NIC (3 TX and 3
RX). It then takes the CPU sets from the NUMA node of the NIC
(`config.auto_cpus`) and the capture NIC from `capture_cfg.sniff_interface`
of `test_config.yaml`.

## Unit tests

```bash
cd tests/acceptance
venv/bin/python -m pytest --noconftest -q stability/tests
```

These tests need no NIC, no root and no EBU LIST.
