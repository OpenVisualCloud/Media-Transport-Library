# 5. CBC FFmpeg-MXL container and test clips

The workload is the CBC FFmpeg-MXL container: a reference DMF media processing
unit that uses MXL shared-memory flows for zero-copy inter-container data
exchange. In the DMF model, a *stream* consists of two co-located Pods — a
decoder and an encoder — that communicate through a shared `/dev/shm` mount
rather than a network socket. This is the pattern AMWA DMF specifies for
co-located media processing containers.

A decoder writes uncompressed frames into an MXL shared-memory flow, and an
encoder reads them from the same memory and encodes H.264 in real time. One
decoder plus one encoder is one *stream* (also called a session, `s01`, `s02`, …).

After building this container you will have the image needed for
[07-container-deployment.md](07-container-deployment.md) and
[08-profiling-manifest.md](08-profiling-manifest.md).

> **Troubleshooting first:** Proxy/API troubleshooting is centralized in
> [02-kubernetes-install-troubleshooting.md](02-kubernetes-install-troubleshooting.md).
> If needed, set `LAB_HTTP_PROXY`/`LAB_HTTPS_PROXY` in `config/lab.env`. Scripts
> construct `NO_PROXY` for cluster addresses; manual `kubectl` commands may still
> require shell `no_proxy`/`NO_PROXY`.

## Build the image

```bash
scripts/build-ffmpeg-mxl-image.sh              # LAB_DEFAULT_NODE
scripts/build-ffmpeg-mxl-image.sh <node>
```

15–30 minutes on a cold cache. It builds **on the worker** from CBC's public
guidance repo and imports the result straight into the containerd namespace the
kubelet uses:

The script installs the `nerdctl` build frontend and the BuildKit daemon when
they are missing, starts `buildkit.service`, and verifies both before beginning
the image build. It extracts only those tools from the nerdctl full archive;
the Kubernetes-managed containerd and runc binaries are left untouched.

The pinned CBC production profile is intentionally minimal and enables MP4/MOV
output but omits MP4/MOV input. This lab consumes MP4, so the build script adds
`--enable-demuxer=mov` to that pinned profile before compiling. Its final check
requires both the MP4/MOV demuxer and MXL muxer.

| Setting | Value | Note |
|---|---|---|
| `CBC_REPO` | `https://github.com/cbcrc/guidance-for-building-ffmpeg-with-mxl.git` | CBC's build guidance for FFmpeg with MXL |
| `CBC_REF` | `9b5098a` | Pinned. Bump deliberately — a different FFmpeg or MXL revision changes encoder cost and therefore every density number. |
| `LAB_IMAGE` | `localhost/mxl-ffmpeg-full:v1` | from `config/lab.env` |
| `LAB_IMAGE_PULL_POLICY` | `Never` | a run can never silently pick up a different build from a registry |
| containerd namespace | `k8s.io` | anything else is invisible to the kubelet |

**Behind a proxy** there is nothing extra to do, but it is worth knowing why: a
build container inherits none of the node's proxy configuration — not
`/etc/environment`, not containerd's drop-in — so the script passes
`LAB_HTTP_PROXY`/`LAB_HTTPS_PROXY` from `config/lab.env` into the build as build
args. BuildKit also pulls each base image before a Dockerfile command runs, so
the script writes the same proxy to a dedicated `buildkit.service` drop-in. It
says so when it does:

```
   via the proxy http://<proxy>:<port>, passed into the build as build args
```

Without that, a proxied build stops at its first `apt-get` and stays there until the
connection times out — twenty minutes in, with the failure looking like a broken
Dockerfile.

The last stage verifies the built image really has MXL, because an FFmpeg that
built successfully without the MXL muxer would fail only at run time:

```bash
nerdctl --namespace k8s.io run --rm --entrypoint /opt/bin/ffmpeg \
  localhost/mxl-ffmpeg-full:v1 -hide_banner -muxers | grep ' mxl '
```

The image's own `ENTRYPOINT` is CBC's RTSP helper; the lab Pods override it with
an explicit command, so only `/opt/bin/ffmpeg` matters.

## Stage the test clips

No media is shipped with this repo. You need one 1080p60 clip; a 4K clip is
optional.

```bash
scripts/stage-media.sh /path/to/clip.mp4              # to LAB_DEFAULT_NODE
scripts/stage-media.sh /path/a.mp4 /path/b.mp4 -- <node>
scripts/stage-media.sh                                # list what is already there
```

Clips are installed into `LAB_MEDIA_HOSTPATH` (`/opt/mxl-media`) on the worker and
mounted read-only at `/media` in the decoder Pod. Point `config/lab.env` at your
file names:

```bash
LAB_INPUT_1080P=Mixed_40sec_1920x1080_60fps_8bit_420_crf23_veryslow.mp4
LAB_INPUT_4K=Mixed_40sec_3840x2160_60fps_10bit_420_crf23_veryslow.mp4
```

After copying, `stage-media.sh` probes `LAB_INPUT_1080P` with `/opt/bin/ffprobe`
from the exact workload image. A missing image feature or malformed clip fails
here instead of timing out during Pod readiness. These local verification
containers use host networking so nerdctl does not create a bridge that can
overlap the Kubernetes Pod network.

The reference clip is 40 s of mixed content, 1920×1080, 60 fps, 8-bit 4:2:0. The
decoder loops it forever (`-stream_loop -1 -re`), so a short clip is fine — but
content complexity changes encoder cost, so use the same clip when comparing
machines.

## What actually runs

Per stream, two Pods on the same worker:

**Decoder** — reads the file at real time, converts to v210 (uncompressed
10-bit 4:2:2) and publishes an MXL flow:

```
ffmpeg -stream_loop -1 -re -i /media/$INPUT_FILE -c:v v210 \
       -progress /run/mxl/progress -f mxl -video_flow_id $VIDEO_ID /dev/shm/mxl
```

**Encoder** — waits for the flow file to appear, then encodes it live:

```
ffmpeg -f mxl -grain_index_init 1 -on_too_late 0 -i /dev/shm/mxl/$VIDEO_ID.mxl-flow \
       -threads 15 -vf format=yuv420p -c:v libx264 -preset medium -tune zerolatency \
       -bf 0 -rc-lookahead 0 -g 30 -sc_threshold 0 \
       -x264-params sliced-threads=0:slices=2:threads=15 \
       -maxrate 12M -bufsize 1M -progress /run/mxl/progress -f null -
```

`-f null -` discards the output: this measures encode capacity, not disk. The
exact command line of every Pod is saved with each run in
`ffmpeg-commandlines.json`.

Why these flags:

* **MXL over `/dev/shm/mxl`** — zero-copy handoff between the two Pods. No
  network, no encode/decode round-trip, no CNI in the data path. Both Pods
  `hostPath`-mount the same tmpfs directory.
* **`-tune zerolatency -bf 0 -rc-lookahead 0`** — live behaviour: no B-frames, no
  lookahead buffer, so a slow frame is immediately visible as an FPS drop instead
  of being absorbed.
* **`-g 30 -sc_threshold 0`** — fixed 0.5 s GOP, no scene-cut reordering, so cost
  per frame is stable and runs are comparable.
* **`-preset medium`** — realistic broadcast quality. Faster presets inflate the
  stream count and hide the platform effects being studied.
* **`slices=2`, `sliced-threads=0`** Increase parralelisation of encoder to utilise resources
  in low-latency usecase

## FPS measurement

Each Pod has a tiny sidecar ([observability/fps_exporter.py](../observability/fps_exporter.py))
that tails FFmpeg's `-progress` file and exposes it to Prometheus as
`mxl_ffmpeg_fps`, `mxl_ffmpeg_speed_ratio`, `mxl_ffmpeg_drop_frames_total` and
friends. No dependencies, ~5 lines of work per scrape, and it reads a file the
encoder writes anyway — so measuring costs nothing measurable.

A run **passes** when *every* encoder holds at least `LAB_MIN_FPS` (59.5) across
the whole measurement window. One slow stream fails the run.

## Verify

```bash
ssh <user>@<worker> "ls -l /opt/mxl-media; ls -ld /dev/shm/mxl; df -h /dev/shm"
ssh <user>@<worker> "sudo nerdctl --namespace k8s.io images | grep mxl-ffmpeg-full"
```

Next: [06-observability.md](06-observability.md).
