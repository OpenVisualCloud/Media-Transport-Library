# IPMX Support

This guide describes the IPMX (VSF TR-10) support in the Media Transport Library (MTL):
what it does, what it assumes, how to build and run it, and how to write an application
that sends or receives IPMX streams with the MTL API.

IPMX support is an MVP. It covers IPMX senders and receivers of uncompressed video
(ST 2110-20) and of JPEG XS compressed video (ST 2110-22). It lives on the `ipmx-mvp`
branch and is built only when MTL is configured with `-Denable_ipmx=true`. A library
built without the option behaves exactly as upstream MTL.

MTL is a software component. It cannot itself be "IPMX Certified": a product built on MTL is
what gets certified. With the restrictions in this guide, the streams MTL sends conform to
the sender requirements of VSF TR-10-1, TR-10-2, TR-10-9, TR-10-11 and TR-10-15 Part 1, as
checked with the VSF TR-10 TP-1 test scripts (sections 13.1 to 13.3, 13.5, 13.6 and 19.3).

## 1. What is supported

### 1.1 Uncompressed video, ST 2110-20

| Item | Supported |
| --- | --- |
| Resolutions and rates | 1920x1080 at 50, 59.94, 60 fps; 3840x2160 at 59.94, 60 fps, progressive |
| Formats on the wire | YUV 4:2:2 10-bit, RGB 4:4:4 8-bit, RGB 4:4:4 10-bit |
| Session types | `st20` (frame level; slice level is not tested) and the `st20p` pipeline |
| Sender Reports | RTCP Sender Report with SDES CNAME and the IPMX Info Block (Media Info Block 0x0001) before every frame, on `udp_port + 1` (TR-10-1 section 8.7, TR-10-2 section 10) |
| SDP | built by the library: `ipmx_st20_tx_get_sdp()` (TR-10-1 section 10, TR-10-2 section 7) |
| Receiver | parses the Sender Reports of each port, `ipmx_st20_rx_get_sr_info()`; optional TR-10-1 section 8.1 CINST / VRX timing check, `ipmx_st20_rx_get_tp_stat()` |
| DSCP | AF42 on the media packets (TR-10-1) |

Other progressive frame rates MTL supports are accepted, but only the rates above were
tested. 2160p needs a 25 GbE or faster port.

### 1.2 JPEG XS, ST 2110-22

| Item | Supported |
| --- | --- |
| Profile | IPMX-JPEG-XS of VSF TR-10-15 Part 1 section 8: High444.12, RGB 4:4:4, codestream packetization (K = 0), sequential transmission (T = 1), constant bit rate (TR-10-11) |
| Formats | RGB 4:4:4 8-bit (`GBRPLANAR8`, the TR-10-15 minimum) and 10-bit (`GBRPLANAR10LE`, an additional bit depth TR-10-15 Part 1 section 8 allows), on TX and RX |
| Resolutions and rates | as for ST 2110-20 |
| Bits per pixel | up to 4 bpp (Sublev3bpp, Sublev4bpp), tested at 3 and 4 bpp |
| Codec | SVT-JPEG-XS through its MTL plugin, see section 3.3 |
| Session types | `st22` (frame level) and the `st22p` pipeline (JPEG XS only) |
| Sender Reports | IPMX Info Block with the Media Info Blocks 0x0003 (TR-10-11 section 12) and 0x0008 (TR-10-15 Part 1 section 9) |
| SDP | `video/jxsv` SDP built by the library: `ipmx_st22_tx_get_sdp()` |
| Codestream check | the TX computes the profile (Ppih) and level (Plev) at create and drops, with a counter, a frame whose codestream header disagrees |

### 1.3 Common

* **ST 2022-7.** Both legs send their own Sender Reports, with the same Info Block. The SDP
  has `a=group:DUP` and one media section per leg. The receiver reads each leg separately.
* **PTP.** The `ts-refclk` of the SDP and the Info Block is
  `ptp=IEEE1588-2008:<grandmaster>:<domain>` while the MTL built-in PTP follows a
  grandmaster, else `localmac=<primary port MAC>`. An application that runs its own PTP
  (linuxptp) sets the string itself, see section 5.6.
* **Without PTP.** This is the default and the mode most of the validation ran in. Without
  `MTL_FLAG_PTP_ENABLE` (RxTxApp without `--ptp`) MTL takes its time from the host system
  clock, and the SDP and the Info Block carry `ts-refclk:localmac=<primary port MAC>`
  (TR-10-1 section 10.4). The streams pass the VSF scripts with the no-PTP stream profile
  (`PTP=false`), TR-10-9 section 11.2 timing and the TR-10-1 NTP-rate SHOULD included. A
  grandmaster on the network is ignored unless PTP is enabled. An MTL receiver takes streams
  from senders with or without PTP; it reads their Sender Reports but does no timing recovery
  from them (TR-10-9 section 11.1), which is left to the application.
* **Bindings.** Python (`pyipmx`) and Rust (`imtl-sys` feature `ipmx`), see section 5.9.

## 2. Design choices and assumptions

These choices shape what an application must do. They are deliberate; section 7 lists the
limitations that follow from them.

1. **The library builds the SDP, the application publishes it.** The SDP values come from the
   same state as the Sender Reports, so the two cannot disagree. The library writes no file
   and has no callback: poll `ipmx_*_tx_get_sdp()` and republish when its `o=` session version
   changes.
2. **The application owns the Info Block block version.** TR-10-1 section 8.7 ties the version
   to the Info Block content. MTL keeps no state across sessions: when an application
   re-creates a session with parameters that change the Info Block, it sets the version of
   the new session to the old one + 1, wrapping at 255, before the first frame. The library
   bumps the version itself only when the `ts-refclk` changes.
3. **Non-baseband sender.** MTL senders are not based on a baseband signal: `htotal` is the
   width, `vtotal` the height and `measuredpixclk` width x height x rate. Colorimetry BT709,
   TCS SDR, range NARROW and pixel aspect ratio 1:1 are fixed.
4. **Media clock `direct=0`.** RTP timestamps follow the PTP (TAI) time the library runs on.
   With `ST20_TX_FLAG_USER_TIMESTAMP` the application must pass timestamps on that time base.
5. **Rate-limit (RL) pacing.** The IPMX timing (TR-10-9 section 11.2, TR-10-1 section 8.1) was
   met and validated with the NIC rate-limit pacing of E810 / E830 NICs, which
   `ST21_TX_PACING_WAY_AUTO` selects there. TSC pacing is not validated for IPMX.
   `ST21_PACING_LINEAR` is rejected with IPMX.
6. **JPEG XS needs isolated MTL CPUs.** The SVT-JPEG-XS encoder and decoder threads float over
   every CPU of the process. When they run on a CPU of an MTL scheduler they delay the TX by
   milliseconds, which breaks TR-10-9 section 11.2 and can drop frames at the receiver. Keep
   the MTL lcores free of other threads, see [isolation.md](isolation.md).
7. **JPEG XS encoder settings are per process.** The SVT-JPEG-XS plugin reads one config file
   per process (section 3.3), so one process sends either 1080p or 2160p IPMX JPEG XS streams,
   at one bpp.
8. **IPMX receivers accept fewer plain ST 2110 senders.** An IPMX receiver rejects odd UDP ports
   and ST 2110 RTCP retransmission; a stream without Sender Reports is still received.

## 3. Get the code and build

### 3.1 Repositories

| Component | Repository | Branch or commit |
| --- | --- | --- |
| MTL with IPMX | <https://github.com/OpenVisualCloud/Media-Transport-Library> | branch `ipmx-mvp` |
| SVT-JPEG-XS and its MTL plugin (JPEG XS only) | <https://github.com/OpenVisualCloud/SVT-JPEG-XS> | commit `2dc3564845f7d4377dc550b9b9bba199960d8683` (2026-11-09) |
| VSF IPMX test scripts (validation only) | <https://github.com/alabou/nmos-testing> | branch `testing-IPMX` |

The SVT-JPEG-XS release v0.10.0 is not enough: the IPMX plugin config files, the encoder
`bpp` key (commit `6ca4f0d`) and the `GBRPLANAR8` format (commit `93f1f20`) came after it.
Use commit `2dc3564`, the one the results of section 6.2 were taken with. It also has the
encoder and decoder speed-ups that came after `93f1f20`.

```bash
git clone -b ipmx-mvp https://github.com/OpenVisualCloud/Media-Transport-Library.git
git clone https://github.com/OpenVisualCloud/SVT-JPEG-XS.git
git -C SVT-JPEG-XS checkout 2dc3564845f7d4377dc550b9b9bba199960d8683
```

### 3.2 MTL

Prepare the host and build DPDK as [build.md](build.md) describes (DPDK and ICE driver
versions in `versions.env`). Then configure the library build directory with IPMX before the
first `./build.sh`:

```bash
cd Media-Transport-Library
meson setup build -Denable_ipmx=true
./build.sh
pkg-config --variable=ipmx mtl    # prints "true"
```

`./build.sh` keeps the option of an existing `build/`. It installs `libmtl` with
`include/mtl/ipmx/ipmx_api.h` and the pkg-config variable `ipmx=true`, then builds RxTxApp,
which turns its IPMX support on when it finds that variable.

On a tree already built without IPMX, the other build directories keep the pkg-config result
they read before. Reconfigure the library and remove them:

```bash
meson configure build -Denable_ipmx=true
rm -rf tests/tools/RxTxApp/build build/tests
./build.sh
```

### 3.3 SVT-JPEG-XS and the MTL plugin (JPEG XS only)

Build MTL first: the plugin offers `GBRPLANAR8`, the 8-bit IPMX JPEG XS input format, only
when it is built against an MTL that has `ST_FRAME_FMT_GBRPLANAR8`, which is on the `ipmx-mvp`
branch. It always offers `GBRPLANAR10LE`, the 10-bit one. The same IPMX config files serve
both bit depths: the encoder writes the component bit depth of its input.

```bash
cd SVT-JPEG-XS/Build/linux
./build.sh install
cd ../../mtl-plugin
./build.sh                     # release build, installs with sudo ninja install
```

The plugin build finds MTL (26.09 or newer) and SVT-JPEG-XS (0.10 or newer) through
pkg-config, not through a source directory. Both install to `/usr/local` by default, which
pkg-config searches. If either is installed to another prefix (`MTL_INSTALL_PREFIX`, or
`./build.sh --prefix` of SVT-JPEG-XS), add its pkg-config directory first, and check that the
build sees the IPMX MTL:

```bash
export PKG_CONFIG_PATH=<mtl prefix>/lib/x86_64-linux-gnu/pkgconfig:<svt prefix>/lib/pkgconfig:$PKG_CONFIG_PATH
pkg-config --variable=ipmx mtl    # "true"
```

Enable the plugin in the MTL plugin registry (`kahawai.json`) and point MTL at it with
`KAHAWAI_CFG_PATH`:

```json
{
  "plugins": [
    {
      "enabled": 1,
      "name": "st22_svt_jpeg_xs",
      "path": "/usr/local/lib/x86_64-linux-gnu/libst_plugin_st22_svt_jpeg_xs.so"
    }
  ]
}
```

The plugin makes IPMX codestreams only with one of its IPMX config files, named by
`SVT_JXS_MTL_PLUGIN_CONFIG`. Without one it uses its default settings, which do not meet
VSF TR-08 (Main profiles, Rl = 1, deadzone quantizer in SPEED mode).

| File | Streams | `stream_level` |
| --- | --- | --- |
| `mtl-plugin/ipmx_1080p.json` | 1920x1080 up to 60 fps, up to 3 bpp | `0x1004`: 2k-1, Sublev3bpp |
| `mtl-plugin/ipmx_2160p.json` | 3840x2160 up to 60 fps, up to 3 bpp | `0x2404`: 4k-2, Sublev3bpp |

For 4 bpp, the rate of the TR-08 Appendix B RGB interoperability points, copy the file and set
the Sublev4bpp level and the rate: `"stream_level": "0x1006", "bpp": 4` (1080p) or
`"stream_level": "0x2406", "bpp": 4` (2160p). The plugin README (`mtl-plugin/README.md`,
section 3.1) has the details.

```bash
export KAHAWAI_CFG_PATH=/path/to/kahawai.json
export SVT_JXS_MTL_PLUGIN_CONFIG=/path/to/SVT-JPEG-XS/mtl-plugin/ipmx_1080p.json
```

### 3.4 Python and Rust bindings

`./script/build_python.sh` builds `pymtl` and, when `pkg-config --variable=ipmx mtl` is `true`,
the `pyipmx` module of `python/swig/ipmx`. Rust: `imtl-sys` with the `ipmx` feature, as a path
dependency; it fails to build against an MTL without IPMX. See
[python/README.md](../python/README.md) section 4 and [rust/README.md](../rust/README.md).

## 4. Run the samples with RxTxApp

Set up the host as for any MTL application ([run.md](run.md)): hugepages, VFs bound to
`vfio-pci`, MtlManager. For JPEG XS also isolate the MTL lcores (section 2, item 6).

`tests/tools/RxTxApp/script/ipmx_json/` has two configs that send one stream to a receiver in
the same process: `st20p_rgb8_1080p59.json` and `st22p_rgb8_1080p59.json`. Edit the interface
names and addresses, and give an input file of raw frames in the `input_format` of the config.
The IPMX keys are `"ipmx": true` on a TX or RX `st20p` / `st22p` session and, on TX,
`"ipmx_block_version"` (0 to 255); see [configuration_guide.md](configuration_guide.md).

```bash
# ST 2110-20, RGB 8-bit 1080p59.94
./tests/tools/RxTxApp/build/RxTxApp \
  --config_file tests/tools/RxTxApp/script/ipmx_json/st20p_rgb8_1080p59.json \
  --ipmx_sdp_dir /tmp/ipmx_sdp --test_time 30

# JPEG XS, 3 bpp, with the MTL lcores on isolated CPUs
KAHAWAI_CFG_PATH=/path/to/kahawai.json \
SVT_JXS_MTL_PLUGIN_CONFIG=/path/to/SVT-JPEG-XS/mtl-plugin/ipmx_1080p.json \
./tests/tools/RxTxApp/build/RxTxApp \
  --config_file tests/tools/RxTxApp/script/ipmx_json/st22p_rgb8_1080p59.json \
  --ipmx_sdp_dir /tmp/ipmx_sdp --lcores 2,3,4,5 --test_time 30
```

* Each IPMX TX session writes its SDP to `<ipmx_sdp_dir>/<session name>.sdp` (for example
  `app_tx_st20p_0.sdp`) and rewrites it when the content changes. Give that file to the
  receiver or to the validation scripts.
* Each IPMX RX session prints one line per port at the end, for example:

  ```text
  app_ipmx_rx_st20p_result(0), ipmx sr port 0, received 1859 malformed 0 info_malformed 0 no_info_block 0 version_changes 0, version 0, ts_refclk localmac=12-75-F8-07-CC-DC, cname 192.168.0.8, OK
  ```

  `OK` means Sender Reports arrived, all with a valid IPMX Info Block.
* `--ptp` turns on the built-in PTP, so `ts-refclk` carries the grandmaster.
  `--ipmx_ts_refclk <str>` sets the `ts-refclk` of every TX session, for a host that runs
  linuxptp; the SDP file is written once the Sender Reports carry it.
* `--rx_timing_parser` adds the CINST / VRX line of the RX. With the software RX timestamps of
  RxTxApp it is not a verdict.

## 5. Writing an IPMX application

Include the IPMX header next to the MTL headers and link `libmtl` as usual:

```c
#include <mtl/mtl_api.h>
#include <mtl/st_pipeline_api.h>
#include <mtl/ipmx/ipmx_api.h>
```

Guard IPMX code with the pkg-config variable when the application must also build against an
MTL without IPMX: RxTxApp defines `APP_HAS_IPMX` when `pkg-config --variable=ipmx mtl` is
`true`. In an MTL without IPMX the flags exist, but a create with one of them fails, and the
`ipmx_*` functions are not in the library.

### 5.1 Session flags

| Session | TX flag | RX flag |
| --- | --- | --- |
| `st20` | `ST20_TX_FLAG_IPMX` | `ST20_RX_FLAG_IPMX` |
| `st20p` | `ST20P_TX_FLAG_IPMX` | `ST20P_RX_FLAG_IPMX` |
| `st22` | `ST22_TX_FLAG_IPMX` | `ST22_RX_FLAG_IPMX` |
| `st22p` | `ST22P_TX_FLAG_IPMX` | `ST22P_RX_FLAG_IPMX` |

A create with the flag returns NULL, and logs the reason, when the library has no IPMX or the
ops break an IPMX rule:

* an odd UDP port, or one of 1024 or less (the Sender Reports use `udp_port + 1`); the same
  check runs on `*_update_destination()` and `*_update_source()`;
* `ST21_PACING_LINEAR`, RTP level sessions, interlaced video;
* the ST 2110 RTCP retransmission flags (`ST20_TX_FLAG_ENABLE_RTCP` and the like);
* ST 2110-20: the `ST20_FMT_V210` and `ST20_FMT_YUV_422_PLANAR10LE` formats;
* ST 2110-22: `ST22_TX_FLAG_DISABLE_BOXES` / `ST22_RX_FLAG_DISABLE_BOXES`, slice packing;
  `st22` TX also needs `fmt` `ST20_FMT_RGB_8BIT` or `ST20_FMT_RGB_10BIT`, the bit depth of
  its codestreams (the default 0 is refused); `st22p` TX needs `ST22_CODEC_JPEGXS`,
  `GBRPLANAR8` or `GBRPLANAR10LE` input, and sets that `fmt` itself; both need a codestream
  size of at most 4 bpp.

### 5.2 Sending ST 2110-20

```c
struct st20p_tx_ops ops;
memset(&ops, 0, sizeof(ops));
ops.name = "ipmx_tx";
ops.port.num_port = 1;
/* port name, destination IP: as for any st20p session */
ops.port.udp_port[MTL_SESSION_PORT_P] = 20000; /* even and > 1024 */
ops.port.payload_type = 112;
ops.width = 1920;
ops.height = 1080;
ops.fps = ST_FPS_P59_94;
ops.input_fmt = ST_FRAME_FMT_RGB8;
ops.transport_fmt = ST20_FMT_RGB_8BIT;
ops.framebuff_cnt = 3;
ops.flags = ST20P_TX_FLAG_BLOCK_GET | ST20P_TX_FLAG_IPMX;

st20p_tx_handle tx = st20p_tx_create(mtl, &ops);
if (!tx) return -EIO; /* the log says which IPMX rule failed */

/* a re-created stream with changed parameters: old version + 1, before the first frame */
ipmx_st20p_tx_set_block_version(tx, block_version);

char sdp[IPMX_SDP_MAX];
int len = ipmx_st20p_tx_get_sdp(tx, sdp, sizeof(sdp));
if (len > 0) publish_sdp(sdp); /* your management interface, e.g. NMOS IS-05 */

/* then the usual st20p_tx_get_frame() / st20p_tx_put_frame() loop */
```

The library sends the Sender Report of each frame itself. Nothing else is needed per frame.

### 5.3 Keeping the SDP current

The SDP can change while the session runs: the `ts-refclk` follows the built-in PTP, and a
block version or destination update changes it too. There is no callback. From a control
thread, poll once a second or so, and republish when the content changes:

```c
struct ipmx_video_info info;
if (ipmx_st20p_tx_get_info(tx, &info) == 0 && info.version != last_version) {
  last_version = info.version;
  len = ipmx_st20p_tx_get_sdp(tx, sdp, sizeof(sdp));
  if (len > 0) publish_sdp(sdp);
}
```

Comparing the `o=` session version of the SDP works as well: it goes up by 1 whenever a call
finds the content changed. TR-10 TP-1 sections 13.2 and 13.3 compare the published SDP with
the Sender Reports, so a stale SDP fails them.

### 5.4 Sending JPEG XS

The same steps with the `st22p` calls. The codec settings come from the plugin config file of
the process (section 3.3); the session gives the rate through the codestream size.

```c
struct st22p_tx_ops ops;
memset(&ops, 0, sizeof(ops));
/* name, port, udp_port (even, > 1024), payload_type as for st20p */
ops.width = 1920;
ops.height = 1080;
ops.fps = ST_FPS_P59_94;
ops.input_fmt = ST_FRAME_FMT_GBRPLANAR8; /* planes G, B, R; or GBRPLANAR10LE */
ops.pack_type = ST22_PACK_CODESTREAM;
ops.codec = ST22_CODEC_JPEGXS;
ops.device = ST_PLUGIN_DEVICE_AUTO;
ops.quality = ST22_QUALITY_MODE_QUALITY;
ops.codestream_size = ops.width * ops.height * 3 / 8; /* 3 bpp */
ops.framebuff_cnt = 3;
ops.flags = ST22P_TX_FLAG_BLOCK_GET | ST22P_TX_FLAG_IPMX;

st22p_tx_handle tx = st22p_tx_create(mtl, &ops);
char sdp[IPMX_SDP_MAX];
int len = ipmx_st22p_tx_get_sdp(tx, sdp, sizeof(sdp)); /* video/jxsv */
```

Frames in packed `RGB8` convert to `GBRPLANAR8` with `st_frame_convert()`; the st22 pipeline
does not convert. The receiver can decode into `GBRPLANAR8` or `GBRPLANAR10LE` (or
`YUV444PLANAR*`, which then holds the planes R, G, B).

With `GBRPLANAR10LE` the stream is RGB 4:4:4 10-bit: the Sender Report, the `jpvi` box and the
SDP (`depth=10`) say 10, and the codestream must carry B = 10. The frame-level `st22` session
takes the same from `ops.fmt = ST20_FMT_RGB_10BIT` (`ST20_FMT_RGB_8BIT` for 8-bit); a frame
whose JPEG XS header has the other bit depth is not sent and is counted.

### 5.5 Receiving

Set the RX flag and read the Sender Reports of each port when needed. The media RX never
depends on them: a stream without Sender Reports, from a plain ST 2110 sender, is received as
without the flag.

```c
ops_rx.flags |= ST20P_RX_FLAG_IPMX;
st20p_rx_handle rx = st20p_rx_create(mtl, &ops_rx);

struct ipmx_rx_sr_info sr;
int ret = ipmx_st20p_rx_get_sr_info(rx, MTL_SESSION_PORT_P, &sr);
if (ret == 0 && sr.has_video_mib) {
  /* sr.info: width, height, rate, sampling, depth, colorimetry, ts_refclk, version */
}
/* -EBUSY: no Sender Report yet. Poll sr.version_changes to see a new Info Block. */
```

* `ipmx_st22p_rx_get_sr_info()` also gives the JPEG XS Media Info Block in `sr.jxs`
  (`has_jxs_mib`): profile, level, packetization.
* The Sender Reports arrive on `udp_port + 1`. Each port takes one more RX queue for them,
  unless MTL runs with the CNI queue (`MTL_FLAG_RX_USE_CNI`, or a CNI tasklet). Count it when
  you set the RX queue count.
* The library does no timing recovery from the Sender Reports. The NTP timestamp fields are
  the raw PTP time of the sender.
* With `ST20P_RX_FLAG_TIMING_PARSER_STAT` the RX also checks the sender against the TR-10-1
  section 8.1 network compatibility (CINST) and virtual receive buffer (VRX) models:
  `ipmx_st20p_rx_get_tp_stat()`. The result is meaningful only with NIC RX timestamps.

### 5.6 PTP

With the built-in PTP (`MTL_FLAG_PTP_ENABLE`), the library fills `ts-refclk` with the
grandmaster identity and domain once the primary port has a master it exchanged delays with
(domain 0 to 127, Announce younger than 3 announce intervals), and goes back to `localmac`
when that stops. Each change bumps the block version and the SDP session version, which is
why the SDP must be polled.

An application that supplies the PTP time itself (`ptp_get_time_fn`, e.g. linuxptp `ptp4l`
and `phc2sys`) sets the string, and again on every change:

```c
/* from pmc "GET PARENT_DATA_SET" and "GET TIME_STATUS_NP" */
ipmx_st20p_tx_set_ts_refclk(tx, "ptp=IEEE1588-2008:00-0F-9B-FF-FE-05-4E-D1:0");
/* NULL or a "localmac=..." string while gmPresent is false */
```

### 5.7 ST 2022-7

Create the session with two ports as for plain ST 2110. Both legs send Sender Reports to their
own destination, with the same Info Block, block version and `ts-refclk` (taken from the
primary port). The SDP holds both legs. On the RX, read each leg with its port.

### 5.8 Re-creating a session

When a session is freed and created again with parameters that change the Info Block (format,
resolution, rate), read the old block version and set the new one before the first frame:

```c
struct ipmx_video_info info;
int ret = ipmx_st20p_tx_get_info(old_tx, &info);
st20p_tx_free(old_tx);
st20p_tx_handle tx = st20p_tx_create(mtl, &new_ops);
if (ret == 0) ipmx_st20p_tx_set_block_version(tx, (uint8_t)(info.version + 1));
```

### 5.9 Python and Rust

```python
import pymtl as mtl
import pyipmx

tx_para.flags |= mtl.ST20P_TX_FLAG_IPMX
st20p_tx = mtl.st20p_tx_create(mtl_handle, tx_para)
ret, sdp = pyipmx.ipmx_st20p_tx_get_sdp(st20p_tx)  # poll it, republish when it changes
```

In Rust the IPMX calls are in the unsafe `imtl-sys` crate with the `ipmx` feature; the safe
`imtl` crate has no IPMX support. In both languages a NULL (`None`) handle crashes the process
as in C: check what a create returns.

## 6. Validation

### 6.1 Test suites

| Tier | Where | What |
| --- | --- | --- |
| Unit | `tests/unit/`, `./build.sh unit` (always with IPMX) | Sender Report and Info Block build and parse, SDP builder, create rules, CINST / VRX model, JPEG XS header checks |
| Integration | `tests/integration_tests/`, KahawaiTest `Ipmx_st20p.*`, `Ipmx_st22p.*` | ST 2110-20 and JPEG XS streams over real VFs, ST 2022-7, frame digests |
| Acceptance | `tests/acceptance/tests/single/ipmx/` | RxTxApp senders captured on the wire and checked with the VSF TR-10 TP-1 scripts |

The acceptance cases run the steps of TP-1 section 13.6 on every point of the matrices of
section 1: the capture starts before the stream (13.1); `sdp_validation_script.py` checks the
SDP (13.2, 13.6.2); `ipmx_raw_validate_pcap.py` or `ipmx_jxsv_validate_pcap.py` with
`--cmax --cfg --sdp` checks 2 seconds of the stream (13.1, 13.3, 13.6.1, 19.3, and the CMAX and
TR-10-9 section 11.2 parts of 13.5). Every SHALL and SHOULD must pass. Further cases cover a
block version change, ST 2022-7 and PTP. Their setup is in
[tests/acceptance/README.md](../tests/acceptance/README.md), section "IPMX".

### 6.2 Results

Lab: Intel E835 100 GbE, VF to VF across two ports, DPDK 26.07, ICE 2.7.6, rate-limit pacing,
SVT-JPEG-XS `2dc3564`, VSF scripts at `86668b5`. Results of 2026-11-10, with the JPEG XS points
rerun on 2026-11-16 and the JPEG XS RGB 10-bit points added on 2026-11-17:

* All 15 ST 2110-20 points and all 20 JPEG XS points (10 RGB 8-bit, 10 RGB 10-bit) pass the
  VSF scripts: every SHALL and SHOULD of the pcap validator, and the SDP script with no
  failure or warning. The jxsv validator checks the bit depth only in the Media Info Block
  (TR-10-1-VP-XVAL); it does not read the `jpvi` `schar` or the codestream B.
* The block version, ST 2022-7 (both legs) and PTP cases pass. With PTP, the lab
  grandmaster appears in the SDP and in the Sender Reports.

* KahawaiTest with IPMX built-in (235 passed) and with it off (200 passed): no failures, and
  no change for non-IPMX sessions. `Ipmx_st22p.*` passes every 8-bit and RGB 10-bit case,
  ST 2022-7 included.
* Performance, both bit depths, with the MTL lcores isolated: 4 x 1080p60 and 1 x 2160p60
  JPEG XS streams per host. RGB 10-bit at 4 x 1080p60 3 bpp and 1 x 2160p60 4 bpp runs at
  the full rate (60 fps, 3660 frames in 60 s per stream), session 0 passes every VSF SHALL
  and SHOULD, and its SDP says `depth=10`.

## 7. Known limitations

| Area | Limitation |
| --- | --- |
| Timing | Validated with rate-limit pacing only. TSC pacing exceeded the TR-10-9 section 11.2 limit in the lab. |
| Timing | JPEG XS needs the MTL lcores on CPUs no other thread uses (section 2, item 6). |
| ST 2022-7 | The legs can start up to about 9 ms apart in the first frames. |
| PTP | The built-in PTP takes the first master it hears: no BMCA, no domain selection. After a grandmaster failover to another master port, `ts-refclk` stays `localmac`. |
| PTP | With `ptp_get_time_fn`, `phc2sys` or the TSC time source, `ts-refclk` stays `localmac` unless the application sets it. There is no `traceable` form. |
| SDP | IPv4 only. No `a=ssrc`, `a=framerate`, PAR or CMAX attributes. The JPEG XS SDP has no `PM`, `fbblevel` or interlace parameters. |
| SDP | The values of a non-baseband sender: `htotal` = width, `vtotal` = height, `measuredpixclk` = width x height x rate. |
| Sender Reports | Without PTP, the TR-10-1 NTP-rate SHOULD of the validators can fail: the sender clock runs free. |
| Receiver | Reads only the first Sender Report and the first video Media Info Block of an RTCP packet. A malformed non-SR part drops the RTCP packet. The video Media Info Block must be 92 octets. |
| Receiver | The VRX check assumes height / vtotal = 1080 / 1125; a sender with other blanking can read as VRX underflow. Only frames a port fully received are judged. |
| JPEG XS | RGB 4:4:4 8-bit and 10-bit only, so an MTL receiver is not a TR-10-15 receiver (that also needs YCbCr 4:2:2 10-bit). Above 3 bpp the plugin config needs the Sublev4bpp edit. Sublev4bpp (0x06) is newer than the 2022 edition of ISO/IEC 21122-2, so some receivers may reject 4 bpp. |
| JPEG XS | At 2160p the receiver loses a few frames while the decoder starts. |
| RxTxApp | Pipeline sessions only. A block version change needs a restart with a new config. Two processes sharing one SDP directory overwrite each other's files. |
| Bindings | The safe Rust crate has no IPMX support; there are no binding examples. |
| Data path | The kernel-socket data path does not set DSCP. |

## 8. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `*_tx_create()` / `*_rx_create()` returns NULL | The log names the IPMX rule (section 5.1): usually an odd UDP port, linear pacing or a format IPMX does not take. |
| `pkg-config --variable=ipmx mtl` is empty | MTL was built without `-Denable_ipmx=true` (section 3.2). |
| RxTxApp: `--ipmx_sdp_dir needs RxTxApp built against an IPMX enabled MTL`, or an `"ipmx": true` session fails | RxTxApp was configured before the IPMX library was installed: remove `tests/tools/RxTxApp/build` and run `./build.sh` again. |
| `st22p_tx_create()` fails with IPMX | The input is not `GBRPLANAR8` or `GBRPLANAR10LE`, the codec is not JPEG XS, or the plugin was built against an MTL without `GBRPLANAR8` (section 3.3). |
| `st22_tx_create()` fails with IPMX, `fmt ... not allowed` | `ops.fmt` is not `ST20_FMT_RGB_8BIT` or `ST20_FMT_RGB_10BIT`: set the bit depth of the codestreams the application sends. |
| IPMX JPEG XS TX: `frame dropped, JPEG XS header check fail: component B` | The codestream bit depth is not the session's: a 10-bit encode into an 8-bit session or the reverse. |
| The VSF jxsv validator reports a wrong profile or level | `SVT_JXS_MTL_PLUGIN_CONFIG` is not set, or names the file of the other resolution, or a 3 bpp file for a 4 bpp stream. |
| JPEG XS: TR-10-9 section 11.2 fails, the receiver drops frames | SVT threads run on the MTL scheduler CPUs: isolate the lcores (section 2, item 6). |
| RX line shows `FAILED`, `received 0` | No Sender Report on `udp_port + 1`: the sender is no IPMX sender, a firewall or switch drops the port, or the RX has no queue left for the Sender Reports. |
| A capture misses the start of the stream | A switch with IGMP snooping forwards the group only after the receiver joins. Join the group on the capture interface before the sender starts. |

## 9. References

* VSF TR-10-1 (system timing and definitions), TR-10-2 (uncompressed video), TR-10-9
  (synchronization), TR-10-11 (constant bit rate compressed video), TR-10-15 Part 1 (JPEG XS),
  TR-08 (JPEG XS in ST 2110-22), TR-10 TP-1 (test plan).
* SMPTE ST 2110-10, -20, -21, -22; ST 2022-7; RFC 3550; RFC 9134; ISO/IEC 21122-2.
* [build.md](build.md), [run.md](run.md), [configuration_guide.md](configuration_guide.md),
  [isolation.md](isolation.md), [plugin.md](plugin.md), `include/ipmx/ipmx_api.h`.
