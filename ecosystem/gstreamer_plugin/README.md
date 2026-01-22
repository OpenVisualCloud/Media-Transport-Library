# Gstreamer plugin for MTL

## 1. Building the GStreamer plugins

### 1.1. Prerequisites
Before you begin, ensure you have the following installed on your system:
- `MTL`
- `Meson`
- `gst-plugins-base`
- `gst-plugins-good`
- `gstreamer`
- `gstreamer-devel`

To install the required GStreamer packages on Ubuntu or Debian, run the following commands:

```bash
sudo apt update

sudo apt install -y gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-tools gstreamer1.0-libav libgstreamer1.0-dev
```

> For MTL installation instructions please refer to  [Build Documentation](../../doc/build.md).

### 1.2. Automated Build

To build the GStreamer plugins using the provided script, run the following commands:

```bash
cd Media-Transport-Library/ecosystem/gstreamer_plugin
./build.sh
```

### 1.3. Manual Build Instructions

If you prefer to build the GStreamer plugins manually, follow these steps:

1. Navigate to the GStreamer plugin directory:

    ```bash
    cd Media-Transport-Library/ecosystem/gstreamer_plugin
    ```

2. Set up the build directory with debug build type:

    ```bash
    meson setup --buildtype=debug "$BUILD_DIR"
    ```

3. Set up the build directory (if not already done):

    ```bash
    meson setup "$BUILD_DIR"
    ```

4. Compile the project:

    ```bash
    meson compile -C "$BUILD_DIR"
    ```

By following these steps, you can manually build the GStreamer plugins.

## 2. Running the pipeline

### 2.1. Prerequisites

To run GStreamer plugins, you need an application capable of using the GStreamer plugin API. In the examples provided, we are using `gst-launch-1.0`.

For first-time users, to use `gst-launch-1.0` with our plugins, you need to install the following packages from the package manager. For Ubuntu,
this can be done by running the following command:

```bash
sudo apt-get update
sudo apt-get install gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
```

These packages include:
- **gstreamer1.0-tools**: Provides the `gst-launch-1.0` tool and other GStreamer utilities.
- **gstreamer1.0-plugins-base**: Contains the base set of plugins, which are essential for most GStreamer applications.
- **gstreamer1.0-plugins-good**: Includes a set of well-supported plugins that are generally considered to be of good quality.
By installing these packages, you will have all the necessary tools and plugins to run GStreamer applications and use the `gst-launch-1.0` tool with our plugins.

To run plugins, you need to pass the path to the plugins to your GStreamer application or load them directly.
In the case of `gst-launch-1.0`, the argument for this is `--gst-plugin-path`.
You can also move the plugins to the default plugin directory `$GSTREAMER_PLUGINS_PATH` instead (see [gstreamer-launch documentation](https://gstreamer.freedesktop.org/documentation/tools/gst-launch.html)).

```bash
export GSTREAMER_PLUGINS_PATH=/path/to/your/compiled/plugins
gst-inspect-1.0 --gst-plugin-path $GSTREAMER_PLUGINS_PATH mtl_st20p_rx
```

### 2.2. General arguments
In MTL GStreamer plugins there are general arguments that apply to every plugin.

| Property Name | Type   | Description                                                                                       | Range                    |
|---------------|--------|---------------------------------------------------------------------------------------------------|--------------------------|
| log-level     | uint   | Set the log level (INFO 1 to CRIT 5).                                                             | 1 (INFO) TO 5 (CRITICAL) |
| dev-port      | string | DPDK port for synchronous transmission and reception, bound to the VFIO DPDK driver.              | N/A                      |
| dev-port-red  | string | DPDK port for redundant transmission, bound to the VFIO DPDK driver.                              | N/A                      |
| dev-ip        | string | Local IP address that the port will be identified by. This is the address from which ARP responses will be sent. | N/A       |
| dev-ip-red    | string | Local IP address that the port will be identified by. For the redundant transmission port.        | N/A                      |
| ip            | string | Receiving MTL node IP address.                                                                    | N/A                      |
| ip-red        | string | Receiving MTL node IP address for redundant transmission.                                         | N/A                      |
| udp-port      | uint   | Receiving MTL node UDP port.                                                                      | 0 to G_MAXUINT           |
| udp-port-red  | uint   | Receiving MTL node UDP port for redundant transmission.                                           | 0 to G_MAXUINT           |
| tx-queues     | uint   | Number of TX queues to initialize in DPDK backend.                                                | 0 to G_MAXUINT           |
| rx-queues     | uint   | Number of RX queues to initialize in DPDK backend.                                                | 0 to G_MAXUINT           |
| enable-dma-offload | boolean | Request DMA offload for sessions that support it.                                                | TRUE/FALSE               |
| dma-dev       | string | DPDK port bound for DMA engines, exported as described in `doc/dma.md`. Required when `enable-dma-offload=true`. | N/A                      |
| payload-type  | uint   | SMPTE ST 2110 payload type.                                                                       | 0 to G_MAXUINT           |
| port          | string | Session DPDK device port. If not specified it will be taken from the dev-port argument.           | N/A                      |
| port-red      | string | Redundant session DPDK device port if left open taken from dev-port-red argument if specified.    | N/A                      |

When `enable-dma-offload` is set to `true`, every plugin that supports DMA will request
`ST20P_RX_FLAG_DMA_OFFLOAD` during session creation (currently `mtl_st20p_rx`). Make sure
the `dma-dev` port is bound and exported as described in `doc/dma.md` before enabling the
flag.


> **Warning:**
> Generally, the `log-level`, `dev-port`, `dev-ip`, `tx-queues`, and `rx-queues` are used to initialize the MTLlibrary. As the MTL library handle is shared between MTL
> GStreamer plugins of the same pipeline, you only need to pass them once when specifying the arguments for the firstly initialized pipeline. Nothing happens when you specify them elsewhere;
> they will just be ignored after the initialization of MTL has already happened.

### 2.3. General capabilities

#### 2.3.1. Supported video fps fractions

| Frame Rate | Fraction    |
|------------|-------------|
| 23.98 fps  | `2398/100`  |
| 24 fps     | `24`        |
| 25 fps     | `25`        |
| 29.97 fps  | `2997/100`  |
| 30 fps     | `30`        |
| 50 fps     | `50`        |
| 59.94 fps  | `5994/100`  |
| 60 fps     | `60`        |
| 100 fps    | `100`       |
| 119.88 fps | `11988/100` |
| 120 fps    | `120`       |

#### 2.3.2. Supported Audio Sampling Rates

| Sampling Rate | Value   |
|---------------|---------|
| 44.1 kHz      | `44100` |
| 48 kHz        | `48000` |
| 96 kHz        | `96000` |

#### 2.3.3 PTS controlled pacing

For selected plugins
- SMPTE ST 2110-20 transmission plugin mtl_st20p_tx
- SMPTE ST 2110-30 transmission plugin mtl_st30p_tx
- SMPTE ST 2110-40 transmission plugin mtl_st40p_tx

There is a feature called user-controlled pacing that allows you to control the
pacing of packet transmission using the Presentation timestamp (PTS) of the buffer.
As long as the PTS fits within the pacing window, packets will be sent at the exact
time specified by the PTS.

This feature requires:
- MTL time synchronized with the system clock. If you are using onboard PTP,
you must also synchronize the system's physical clock using `phc2sys`.
- GStreamer buffers to provide a 64-bit timestamp (TUI) since epoch (PTP time).

The following message informs you about the amount of frames that had incorrectly
defined timestamps by the user (this error message should be 0).

```log
 TX_VIDEO_SESSION(0,0): error user timestamp 250
```

When using this feature, you can specify an offset for the packets. The offset
should be large enough to account for any delays between placing the frame in
the framebuffer and the actual transmission of packets onto the wire.

## 3. SMPTE ST 2110-20 Rawvideo plugins

Video plugins for MTL that are able to send, receive synchronous video via the MTL pipeline API.

> **Warning:**
> Raw video plugins require that the buffers passed to them match the size of the video frame.
> Ensure that the buffer size corresponds to the frame size of the video being processed.
>
> **Warning:**
> Keep in mind that raw video files are very large and saving / using them is I/O and memory space intensive.
> Oftentimes pipeline choking point is the drive I/O speed limitation.

### 3.1. Running the SMPTE ST 2110-20 transmission plugin mtl_st20p_tx

#### 3.1.1. Supported parameters and pad capabilities

The `mtl_st20p_tx` plugin supports the following pad capabilities:

- **Formats**: `v210`, `I422_10LE`
- **Width Range**: 64 to 16384*
- **Height Range**: 64 to 8704
- **Framerate Range**: `2398/100`, `24`, `25`, `2997/100`, `30`, `50`, `5994/100`, `60`, `100`,
`11988/100`, `120`

\* Resolution width for v210 format has to be divisible by 3, so the plugin does not support 720p and 1440p.
To be fixed in the future.

[More information about GStreamer capabilities (GstCaps)](https://gstreamer.freedesktop.org/documentation/gstreamer/gstcaps.html)

**Arguments**

| Property Name        | Type     | Description                                           | Range                   | Default Value |
|----------------------|----------|-------------------------------------------------------|-------------------------|---------------|
| retry                | uint     | Number of times the MTL will try to get a frame.      | 0 to G_MAXUINT          | 10            |
| tx-framebuff-num     | uint     | Number of framebuffers to be used for transmission.   | 0 to 8                  | 3             |
| async-session-create | boolean  | Improve initialization time by creating a session in a separate thread. All buffers that arrive before the session is ready will be dropped | TRUE/FALSE              | FALSE         |
| use-pts-for-pacing   | boolean  | [User controlled timestamping offset](#233-pts-controlled-pacing) | 0 to G_MAXUINT | 0          |
| pts-pacing-offset    | uint     | [User controlled timestamping offset](#233-pts-controlled-pacing) | 0 to G_MAXUINT | 0          |

#### 3.1.2. Preparing Input Video

To send the video, you need to have an input video ready.
Here is how to generate an input video with `y210` format using GStreamer.

In the following examples, we will use the `$INPUT` variable to hold the path to the input video.

```bash
export INPUT="path_to_the_input_v210_file"

gst-launch-1.0 -v videotestsrc pattern=ball ! video/x-raw,width=1920,height=1080,format=v210,framerate=60/1 ! filesink location=$INPUT
```

#### 3.1.3. Pipline example for multicast `y210`

To run the raw video transmission plugin, we need to pass the MTL parameters responsible for initializing the MTL library.
To ensure the correct size of the buffer, we will use the `rawvideoparse` element in the case of `y210`.


The rest of rawvideo metadata will be passed via pad capabilities of the buffer.
```bash
# If you don't know how to find VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_T="pci_address_of_the_device"

# video pipeline y210 FHD 60fps on port 20000
gst-launch-1.0 filesrc location=$INPUT ! \
rawvideoparse format=v210 height=1080 width=1920 framerate=25/1 ! \
mtl_st20p_tx tx-queues=4 rx-queues=0 udp-port=20000 payload-type=112 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_T \
--gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

### 3.2. Running the SMPTE ST 2110-20 receiver plugin mtl_st20p_rx

#### 3.2.1. Supported Parameters and Pad Capabilities

The `mtl_st20p_rx` plugin supports the following pad capabilities:

- **Formats**: `v210`, `I422_10LE`
- **Width Range**: 64 to 16384
- **Height Range**: 64 to 8704
- **Framerate Range**: `2398/100`, `24`, `25`, `2997/100`, `30`, `50`, `5994/100`, `60`, `100`,
`11988/100`, `120`

**Arguments**

| Property Name       | Type     | Description                                         | Range                      | Default Value |
|---------------------|----------|-----------------------------------------------------|----------------------------|---------------|
| retry               | uint     | Number of times the MTL will try to get a frame.    | 0 to G_MAXUINT             | 10            |
| rx-fps              | fraction | Framerate of the video.                             | [Supported video fps fractions](#231-supported-video-fps-fractions) | 25/1 |
| rx-framebuff-num    | uint     | Number of framebuffers to be used for transmission. | 0 to 8                     | 3             |
| rx-width            | uint     | Width of the video.                                 | 0 to G_MAXUINT             | 1920          |
| rx-height           | uint     | Height of the video.                                | 0 to G_MAXUINT             | 1080          |
| rx-interlaced       | boolean  | Whether the video is interlaced.                    | TRUE/FALSE                 | FALSE         |
| rx-pixel-format     | string   | Pixel format of the video.                          | `v210`, `YUV444PLANAR10LE` | `v210`        |

> **Tip:** Enable DMA offload for this plugin by passing the general property
> `enable-dma-offload=true` alongside a configured `dma-dev`.

#### 3.2.2. Preparing output path

In our pipelines, we will use the `$OUTPUT` variable to hold the path to the video.

```bash
export OUTPUT="path_to_the_file_we_want_to_save"
```

#### 3.2.3. Pipline example for multicast with `y210` format

To run the `mtl_st20p_rx` plugin, use the following command to specify the input parameters of the incoming stream.

> **Warning**: To receive data, ensure that a transmission is running with the same parameters on the `239.168.75.30` address.

```bash
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_R="pci_address_of_the_device"

# Run the receiver pipeline y210 FHD 60fps on port 20000
gst-launch-1.0 mtl_st20p_rx rx-queues=4 udp-port=20000 payload-type=112 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_R rx-pixel-format=v210 rx-height=1080 rx-width=1920 rx-fps=25/1 ! \
filesink location=$OUTPUT --gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

This command sets up the receiver pipeline with the specified parameters and saves the received video to the specified output path.

## 4. SMPTE ST 2110-30 Raw audio plugins

Audio plugins for MTL that are able to send, receive synchronous raw audio via the MTL pipeline API.

### 4.1. Running the SMPTE ST 2110-30 transmission plugin mtl_st30p_tx

#### 4.1.1. Supported Parameters and Pad Capabilities

The `mtl_st30p_tx` plugin supports the following pad capabilities:

- **Formats**: `S8`, `S16BE`, `S24BE`
- **Sample Rate Range**: 44100, 48000, 96000
- **Channels Range**: 1 to 8

**Arguments**

| Property Name        | Type    | Description                                           | Range                   | Default Value |
|----------------------|---------|-------------------------------------------------------|-------------------------|---------------|
| tx-samplerate        | uint    | Sample rate of the audio.                             | [Supported Audio Sampling Rates](#232-supported-audio-sampling-rates) | 0 |
| tx-channels          | uint    | Number of audio channels.                             | 1 to 8                  | 2             |
| tx-ptime             | string  | Packetization time for the audio stream.              | `1ms`, `125us`, `250us`, `333us`, `4ms`, `80us`, `1.09ms`, `0.14ms`, `0.09ms` | `1.09ms` for 44.1kHz, `1ms` for others |
| async-session-create | boolean | Improve initialization time by creating a session in a separate thread. All buffers that arrive before the session is ready will be dropped | TRUE/FALSE              | FALSE         |
| use-pts-for-pacing   | boolean | [User controlled timestamping offset](#233-pts-controlled-pacing) | 0 to G_MAXUINT | 0          |
| pts-pacing-offset    | uint    | [User controlled timestamping offset](#233-pts-controlled-pacing) | 0 to G_MAXUINT | 0          |

#### 4.1.2. Example GStreamer Pipeline for Transmission with s16BE format

To run the `mtl_st30p_tx` plugin, you need to setup metadata (Here we are using pipeline capabilities).
Instead of using input video we opted for build-in GStreamer audio files generator.

```bash
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_T="pci_address_of_the_device"

# Audio pipeline with 48kHz sample rate on port 30000
gst-launch-1.0 audiotestsrc ! \
audio/x-raw,format=S16BE,rate=48000,channels=2 ! \
mtl_st30p_tx tx-queues=4 rx-queues=0 udp-port=30000 payload-type=113 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_T \
--gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

### 4.2. Running the SMPTE ST 2110-30 Receiver Plugin `mtl_st30p_rx`

#### 4.2.1. Supported Parameters and Pad Capabilities

The `mtl_st30p_rx` plugin supports the following pad capabilities:

- **Formats**: `S8`, `S16BE`, `S24BE`
- **Channels Range**: 1 to 8
- **Sample Rate Range**: 44100, 48000, 96000

**Arguments**

| Property Name       | Type    | Description                                           | Range                    | Default Value |
|---------------------|---------|-------------------------------------------------------|--------------------------|---------------|
| rx-framebuff-num    | uint    | Number of framebuffers to be used for transmission.   | 0 to G_MAXUINT           | 3             |
| rx-channel          | uint    | Audio channel number.                                 | 0 to 8                   | 2             |
| rx-sampling         | uint    | Audio sampling rate.                                  | [Supported Audio Sampling Rates](#232-supported-audio-sampling-rates) | 48000         |
| rx-audio-format     | string  | Audio format type.                                    | `PCM8`, `PCM16`, `PCM24` | `PCM16`       |
| rx-ptime            | string  | Packetization time for the audio stream.              | `1ms`, `125us`, `250us`, `333us`, `4ms`, `80us`, `1.09ms`, `0.14ms`, `0.09ms` | `1.09ms` for 44.1kHz, `1ms` for others |

> **Note**: The `PCM` formats are interpreted as big endian.

#### 4.2.2. Preparing Output Path

In our pipelines, we will use the `$OUTPUT` variable to hold the path to the audio file.

```bash
export OUTPUT="path_to_the_file_we_want_to_save"
```

#### 4.2.3. Example GStreamer pipeline for reception

To run the `mtl_st30p_rx` audio plugin use the command below:

> **Warning**: To receive data, ensure that a transmission is running with the same parameters on the `239.168.75.30` address.

```bash
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_R="pci_address_of_the_device"

# Set the output path
export OUTPUT="path_to_the_file_we_want_to_save"

# Run the receiver pipeline
gst-launch-1.0 -v mtl_st30p_rx rx-queues=4 udp-port=30000 payload-type=111 dev-ip="192.168.96.2" ip="239.168.75.30" dev-port=$VFIO_PORT_R rx-audio-format=PCM16 rx-channel=2 rx-sampling=48000 ! \
filesink location=$OUTPUT --gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

## 5. SMPT SMPTE ST 2110-40 Ancillary Data Plugins

Ancillary data plugins for MTL that are able to send and receive synchronous ancillary data via the MTL API.

### 5.1. Running the SMPTE ST 2110-40 Transmission Plugin `mtl_st40p_tx`

#### 5.1.1. Supported Parameters and Pad Capabilities

The `mtl_st40p_tx` plugin supports all pad capabilities (the data is not checked):

- **Capabilities**: Any (GST_STATIC_CAPS_ANY)

**Arguments**

| Property Name         | Type     | Description                                                        | Range            | Default Value |
|-----------------------|----------|--------------------------------------------------------------------|------------------|---------------|
| tx-framebuff-cnt      | uint     | Number of framebuffers to be used for transmission.                | 0 to G_MAXUINT   | 3             |
| tx-fps                | uint     | Framerate of the video to which the ancillary data is synchronized.| [Supported vid eo fps fractions](#231-supported-video-fps-fractions) | 25/1 |
| tx-interlaced         | boolean  | Treat input as interlaced ANC; set `rx-interlaced=true` on RX to match. | TRUE/FALSE   | FALSE         |
| tx-did                | uint     | Data ID for the ancillary data.                                    | 0 to 255         | 0             |
| tx-sdid               | uint     | Secondary Data ID for the ancillary data.                          | 0 to 255         | 0             |
| tx-split-anc-by-pkt   | boolean  | Emit at most one ANC block per RTP packet; marker on final packet of field. | TRUE/FALSE | FALSE         |
| use-pts-for-pacing    | gboolean | [User controlled timestamping offset](#233-pts-controlled-pacing)  | TRUE/FALSE       | FALSE         |
| pts-pacing-offset     | uint     | [User controlled timestamping offset](#233-pts-controlled-pacing)  | 0 to G_MAXUINT   | 0             |
| input-format          | enum     | Encoding of incoming ANC buffers (`raw-udw`, `rfc8331-packed`, `rfc8331`) | enum            | `raw-udw`     |
| parse-8331-meta       | gboolean | **Deprecated.** Shortcut for `input-format=rfc8331-packed`.         | TRUE/FALSE       | FALSE         |
| max-combined-udw-size | uint     | Maximum combined size of all user data words to send in one buffer | 0 to (20 * 255)  | 20 * 255      |
| tx-test-mode          | enum     | Test-only mutations: `no-marker`, `seq-gap`, `bad-parity`, `paced` (see below). | enum      | none          |
| tx-test-pkt-count     | uint     | Number of packets to emit for `tx-test-mode` patterns (e.g., paced burst). | 0 to G_MAXUINT | 0            |
| tx-test-pacing-ns     | uint     | Inter-packet spacing in nanoseconds for `tx-test-mode=paced`.      | 0 to G_MAXUINT   | 0             |

> **Note:**
> - `raw-udw` streams carry a single ANC packet per frame without per-packet metadata; the element uses `tx-did`/`tx-sdid` for the outgoing headers, so keeping `max-combined-udw-size` small avoids oversized frames.
> - `rfc8331-packed` expects the 10-bit RFC8331 payload (including headers) generated by external tools. This is the mode formerly controlled by `parse-8331-meta`.
> - `rfc8331` accepts the simplified 8-bit representation produced by `mtl_st40p_rx output-format=rfc8331`, enabling metadata round-trips without re-packing parity bits.
> - The `parse-8331-meta` flag remains available for backward compatibility but will be removed in a future release. Update existing pipelines to use `input-format=rfc8331-packed` (or `rfc8331`) to avoid breakage when the legacy boolean disappears.

##### Interlaced ancillary streams

The `p` suffix means pipeline API, not progressive-only. For interlaced ANC, set `tx-interlaced=true` and match the receiver with `rx-interlaced=true`. Example:

```bash
export VFIO_PORT_T="pci_address_of_the_device"
export VFIO_PORT_R="pci_address_of_the_device"

# TX: interlaced ANC paced to 29.97i video
gst-launch-1.0 filesrc location=$INPUT ! \
    mtl_st40p_tx tx-interlaced=true tx-fps=5994/200 tx-did=67 tx-sdid=2 udp-port=40000 payload-type=113 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_T --gst-plugin-path $GSTREAMER_PLUGINS_PATH

# RX: interlaced ANC capture
gst-launch-1.0 -v mtl_st40p_rx rx-interlaced=true udp-port=40000 payload-type=113 dev-ip="192.168.96.2" ip="239.168.75.30" dev-port=$VFIO_PORT_R ! filesink location=$OUTPUT --gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

#### 5.1.2. Example GStreamer Pipeline for Transmission

To run the `mtl_st40p_tx` plugin, use the following command:

```bash
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_T="pci_address_of_the_device"

# Ancillary data pipeline on port 40000
gst-launch-1.0 filesrc location=$INPUT ! mtl_st40p_tx tx-queues=4 udp-port=40000 payload-type=113 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_T  tx-framebuff-cnt=3 tx-fps=25/1 tx-did=67 tx-sdid=2 --gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

This command sets up the transmission pipeline with the specified parameters and sends the ancillary data using the `mtl_st40p_tx` plugin.

#### 5.1.3. ST40P test-mode knobs

Test-only RTP/ANC mutation options for validation (not for production):

| Property              | Description                                                                                       |
|-----------------------|---------------------------------------------------------------------------------------------------|
| `tx-test-mode=no-marker` | Clear the RTP marker bit so RX never reports a ready frame.                                      |
| `tx-test-mode=seq-gap`   | Insert a deliberate RTP sequence hole to validate `seq_discont/seq_lost` accounting.            |
| `tx-test-mode=bad-parity`| Corrupt ANC parity so RX drops metadata instead of surfacing payload.                           |
| `tx-test-mode=paced`     | Emit a bounded packet burst (set `tx-test-pkt-count`) with optional inter-packet spacing (`tx-test-pacing-ns`) to check pacing and accumulation of multi-packet ANC fields. |

Notes:
- These knobs can be combined with `tx-split-anc-by-pkt=true` to stress the split-mode path.
- Pair with RX `frame-info-path` to log packet counts, markers, and sequence discontinuities when validating.

### 5.2. Running the SMPTE ST 2110-40 Receiver Plugin `mtl_st40p_rx`

#### 5.2.1. Supported Parameters and Pad Capabilities

The `mtl_st40p_rx` plugin supports all pad capabilities (the data is not checked):

- **Capabilities**: Any (GST_STATIC_CAPS_ANY)

**Arguments**

| Property Name       | Type    | Description                                                 | Range                       | Default Value |
|---------------------|---------|-------------------------------------------------------------|-----------------------------|---------------|
| rx-framebuff-cnt    | uint    | Number of frame buffers allocated in the pipeline           | 0 to G_MAXUINT              | 3             |
| max-udw-size        | uint    | Maximum user-data word buffer size in bytes per frame       | 1024 to 1,048,576           | 131,072       |
| rtp-ring-size       | uint    | Size of the internal RTP ring (must be power of two)        | 64 to 16,384                | 1,024         |
| timeout             | uint    | Timeout in seconds for blocking frame retrieval             | 0 to 300                    | 60            |
| frame-info-path     | string  | Optional path to append frame info and sequence stats per frame | N/A                      | NULL          |
| rx-interlaced       | boolean | Whether the incoming ancillary stream is interlaced         | TRUE/FALSE                  | FALSE         |
| rx-auto-detect-interlaced | boolean | Enable RTP F-bit based interlace detection when cadence is unknown; leaves `rx-interlaced` optional and will warn if both are false. | TRUE/FALSE | FALSE |
| output-format       | enum    | Serialization format for received ancillary data            | `raw-udw` / `rfc8331`       | `raw-udw`     |

When `output-format` is set to `rfc8331`, each ancillary packet is serialized with an 8-byte
header (C, line number, horizontal offset, S, stream number, DID, SDID, data count) followed by
its user data words so that downstream elements receive complete RFC8331 payloads. Selecting
`raw-udw` delivers only the user-data words exactly as received on the wire.

> **Note:** `rtp-ring-size` values are validated at runtime. If the supplied number is not a power of
> two the element fails to initialize, matching the behavior enforced inside
> `gst_mtl_st40p_rx_start()`.
> **Interlace auto-detect:** When cadence is unknown, set `rx-auto-detect-interlaced=true` and leave `rx-interlaced=false`. The plugin warns if both are false. Sequence discontinuities reset detection; frame-info shows `seq_discont` when it happens and `interlaced` re-populates after new F bits arrive.

#### 5.2.2. Example GStreamer Pipeline for Reception

To run the `mtl_st40p_rx` plugin, use the following command:
> **Warning**: To receive ancillary data, ensure that a transmission is running on the `239.168.75.30` address.

```bash
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_R="pci_address_of_the_device"

# Set the output path
export OUTPUT="path_to_the_file_we_want_to_save"

# Run the receiver pipeline
gst-launch-1.0 -v mtl_st40p_rx rx-queues=4 rx-framebuff-cnt=3 timeout=61 output-format=rfc8331 udp-port=40000 payload-type=113 dev-ip="$IP_PORT_R" ip="$IP_PORT_T" dev-port=$VFIO_PORT_R ! filesink location=$OUTPUT --gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

