# Gstreamer plugin for MTL

## Building the GStreamer plugins

### Prerequisites
Before you begin, ensure you have the following installed on your system:
- `MTL`
- `Meson`
- `gst-plugins-base`
- `gst-plugins-good`
- `gst-plugins-bad`
- `gst-plugins-ugly`
- `gstreamer`
- `gstreamer-devel`

To install the required GStreamer packages on Ubuntu or Debian, run the following commands:

```shell
sudo apt update

sudo apt install -y gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-tools gstreamer1.0-libav libgstreamer1.0-dev
```

> For MTL installation instructions please refer to  [Build Documentation](../../doc/build.md).

### Automated Build

To build the GStreamer plugins using the provided script, run the following commands:

```shell
cd Media-Transport-Library/ecosystem/gstreamer_plugin
./build.sh
```

### Manual Build Instructions

If you prefer to build the GStreamer plugins manually, follow these steps:

1. Navigate to the GStreamer plugin directory:

    ```shell
    cd Media-Transport-Library/ecosystem/gstreamer_plugin
    ```

2. Set up the build directory with debug build type:

    ```shell
    meson setup --buildtype=debug "$BUILD_DIR"
    ```

3. Set up the build directory (if not already done):

    ```shell
    meson setup "$BUILD_DIR"
    ```

4. Compile the project:

    ```shell
    meson compile -C "$BUILD_DIR"
    ```

By following these steps, you can manually build the GStreamer plugins.

## Running the pipeline

### Prerequisites

To run GStreamer plugins, you need an application capable of using the GStreamer plugin API. In the examples provided, we are using `gst-launch-1.0`.

For first-time users, to use `gst-launch-1.0` with our plugins, you need to install the following packages from the package manager. For Ubuntu,
this can be done by running the following command:

```sh
sudo apt-get update
sudo apt-get install gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly
```

These packages include:
- **gstreamer1.0-tools**: Provides the `gst-launch-1.0` tool and other GStreamer utilities.
- **gstreamer1.0-plugins-base**: Contains the base set of plugins, which are essential for most GStreamer applications.
- **gstreamer1.0-plugins-good**: Includes a set of well-supported plugins that are generally considered to be of good quality.
- **gstreamer1.0-plugins-bad**: Contains a set of plugins that are still under development or testing.
- **gstreamer1.0-plugins-ugly**: Includes plugins that might have licensing issues or are not considered to be of the highest quality.
By installing these packages, you will have all the necessary tools and plugins to run GStreamer applications and use the `gst-launch-1.0` tool with our plugins.

To run plugins, you need to pass the path to the plugins to your GStreamer application or load them directly.
In the case of `gst-launch-1.0`, the argument for this is `--gst-plugin-path`.
You can also move the plugins to the default plugin directory `$GSTREAMER_PLUGINS_PATH` instead (see [gstreamer-launch documentation](https://gstreamer.freedesktop.org/documentation/tools/gst-launch.html)).

```sh
export GSTREAMER_PLUGINS_PATH=/path/to/your/compiled/plugins
gst-inspect-1.0 --gst-plugin-path $GSTREAMER_PLUGINS_PATH mtl_st20p_rx
```

### General arguments
In GStreamer plugins there are general arguments that apply to every plugin.

| Property Name | Type   | Description                                                                                       | Range                    |
|---------------|--------|---------------------------------------------------------------------------------------------------|--------------------------|
| log-level     | uint   | Set the log level (INFO 1 to CRIT 5).                                                             | 1 (INFO) TO 5 (CRITICAL) |
| dev-port      | string | DPDK port for synchronous transmission and reception, bound to the VFIO DPDK driver.              | N/A                      |
| dev-ip        | string | Local IP address that the port will be identified by. This is the address from which ARP responses will be sent. | N/A       |
| ip            | string | Receiving MTL node IP address.                                                                    | N/A                      |
| udp-port      | uint   | Receiving MTL node UDP port.                                                                      | 0 to G_MAXUINT           |
| tx-queues     | uint   | Number of TX queues to initialize in DPDK backend.                                                | 0 to G_MAXUINT           |
| rx-queues     | uint   | Number of RX queues to initialize in DPDK backend.                                                | 0 to G_MAXUINT           |
| payload-type  | uint   | SMPTE ST 2110 payload type.                                                                  | 0 to G_MAXUINT           |

These are also general parameters accepted by plugins, but the functionality they provide to the user is not yet supported in plugins.
| Property Name | Type   | Description                                                                                       | Range                    |
|---------------|--------|---------------------------------------------------------------------------------------------------|--------------------------|
| dma-dev       | string | **RESERVED FOR FUTURE USE** port for the MTL direct memory functionality.                         | N/A                      |
| port          | string | **RESERVED FOR FUTURE USE** DPDK device port. Utilized when multiple ports are passed to the MTL library to select the port for the session. | N/A |

### General capabilities

Some structures describe the capabilities generally

#### Supported video fps codes gst_mtl_supported_fps
```c
enum gst_mtl_supported_fps {
  GST_MTL_SUPPORTED_FPS_23_98 = 2398,   // 23.98 fps
  GST_MTL_SUPPORTED_FPS_24 = 24,        // 24 fps
  GST_MTL_SUPPORTED_FPS_25 = 25,        // 25 fps
  GST_MTL_SUPPORTED_FPS_29_97 = 2997,   // 29.97 fps
  GST_MTL_SUPPORTED_FPS_30 = 30,        // 30 fps
  GST_MTL_SUPPORTED_FPS_50 = 50,        // 50 fps
  GST_MTL_SUPPORTED_FPS_59_94 = 5994,   // 59.94 fps
  GST_MTL_SUPPORTED_FPS_60 = 60,        // 60 fps
  GST_MTL_SUPPORTED_FPS_100 = 100,      // 100 fps
  GST_MTL_SUPPORTED_FPS_119_88 = 11988, // 119.88 fps
  GST_MTL_SUPPORTED_FPS_120 = 120       // 120 fps
};
```

#### Supported Audio Sampling Rates gst_mtl_supported_audio_sampling
```c
enum gst_mtl_supported_audio_sampling {
  GST_MTL_SUPPORTED_AUDIO_SAMPLING_44_1K = 44100,  // 44.1 kHz
  GST_MTL_SUPPORTED_AUDIO_SAMPLING_48K = 48000,    // 48 kHz
  GST_MTL_SUPPORTED_AUDIO_SAMPLING_96K = 96000     // 96 kHz
};
```

### SMPTE ST 2110-20 Rawvideo plugins

Video plugins for MTL that are able to send, receive synchronous video via the MTL pipeline API.

> **Warning:**
> Raw video plugins require that the buffers passed to them match the size of the video frame.
> Ensure that the buffer size corresponds to the frame size of the video being processed.

> **Warning:**
> Keep in mind that raw video files are very large and saving / using them is I/O and memory space intensive.
> Oftentimes pipeline choking point is the drive I/O speed limitation.

### Running the SMPTE ST 2110-20 transmission plugin mtl_st20p_tx

#### Supported parameters and pad capabilities

The `mtl_st20p_tx` plugin supports the following pad capabilities:

- **Formats**: `v210`, `I422_10LE`
- **Width Range**: 64 to 16384
- **Height Range**: 64 to 8704
- **Framerate Range**: 24, 25, 30, 50, 60, 100, 120

[More information about GStreamer capabilities (GstCaps)](https://gstreamer.freedesktop.org/documentation/gstreamer/gstcaps.html)

**Arguments**
| Property Name       | Type   | Description                                           | Range                   | Default Value |
|---------------------|--------|-------------------------------------------------------|-------------------------|---------------|
| retry               | uint   | Number of times the MTL will try to get a frame.      | 0 to G_MAXUINT          | 10            |
| tx-fps              | uint   | Framerate of the video.                               | [gst_mtl_supported_fps](#video-formats-gst_mtl_supported_fps) | 0 |
| tx-framebuff-num    | uint   | Number of framebuffers to be used for transmission.   | 0 to 8                  | 3             |

#### Preparing Input Video

To send the video, you need to have an input video ready.
Here is how to generate an input video with `y210` format using GStreamer.

In our pipelines, we will use the `$INPUT` variable to hold the path to the input video.

```shell
export INPUT="path_to_the_input_v210_file"

gst-launch-1.0 -v videotestsrc pattern=ball ! video/x-raw,width=1920,height=1080,format=v210,framerate=60/1 ! filesink location=$INPUT
```

#### Pipline example for multicast `y210`

To run the raw video transmission plugin, we need to pass the MTL parameters responsible for initializing the MTL library.
To ensure the correct size of the buffer, we will use the `rawvideoparse` element in the case of `y210`.


The rest of rawvideo metadata will be passed via pad capabilities of the buffer.
```shell
# If you don't know how to find VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_T="pci_address_of_the_device"

# video pipeline y210 FHD 60fps on port 20000
gst-launch-1.0 filesrc location=$INPUT ! \
rawvideoparse format=v210 height=1080 width=1920 framerate=60/1 ! \
mtl_st20p_tx tx-queues=4 rx-queues=0 udp-port=20000 payload-type=112 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_T \
--gst-plugin-path $GSTREAMER_PLUGINS_PATH

# video pipeline y210 FHD 60fps on port 20000
gst-launch-1.0 multifilesrc location=$INPUT loop=true ! \
rawvideoparse format=v210 height=1080 width=1920 framerate=60/1 ! \
mtl_st20p_tx tx-queues=4 rx-queues=0 udp-port=20000 payload-type=112 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_T \
--gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

### Running the SMPTE ST 2110-20 receiver plugin mtl_st20p_rx

#### Supported Parameters and Pad Capabilities

The `mtl_st20p_rx` plugin supports the following pad capabilities:

- **Formats**: `v210`, `I422_10LE`
- **Width Range**: 64 to 16384
- **Height Range**: 64 to 8704
- **Framerate Range**: 0 to MAX

**Arguments**
| Property Name       | Type    | Description                                         | Range                      | Default Value |
|---------------------|---------|-----------------------------------------------------|----------------------------|---------------|
| retry               | uint    | Number of times the MTL will try to get a frame.    | 0 to G_MAXUINT             | 10            |
| rx-fps              | uint    | Framerate of the video.                             | [gst_mtl_supported_fps](#video-formats-gst_mtl_supported_fps) | 0 |
| rx-framebuff-num    | uint    | Number of framebuffers to be used for transmission. | 0 to 8                     | 3             |
| rx-width            | uint    | Width of the video.                                 | 0 to G_MAXUINT             | 1920          |
| rx-height           | uint    | Height of the video.                                | 0 to G_MAXUINT             | 1080          |
| rx-interlaced       | boolean | Whether the video is interlaced.                    | TRUE/FALSE                 | FALSE         |
| rx-pixel-format     | string  | Pixel format of the video.                          | `v210`, `YUV444PLANAR10LE` | `v210`        |

#### Preparing output path

In our pipelines, we will use the `$OUTPUT` variable to hold the path to the video.

```shell
export OUTPUT="path_to_the_file_we_want_to_save"
```

#### Pipline example for multicast with `y210` format

To run the `mtl_st20p_rx` plugin, use the following command to specify the input parameters of the incoming stream.

> **Warning**: To receive data, ensure that a transmission is running with the same parameters on the `239.168.75.30` address.

```shell
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_R="pci_address_of_the_device"

# Run the receiver pipeline y210 FHD 60fps on port 20000
gst-launch-1.0 mtl_st20p_rx rx-queues=4 udp-port=20000 payload-type=112 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_R rx-pixel-format=v210 rx-height=1080 rx-width=1920 rx-fps=25 ! \
filesink location=$OUTPUT --gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

This command sets up the receiver pipeline with the specified parameters and saves the received video to the specified output path.

### SMPTE ST 2110-30 Raw audio plugins

Audio plugins for MTL that are able to send, receive synchronous raw audio via the MTL pipeline API.

### Running the SMPTE ST 2110-30 transmission plugin mtl_st30p_tx

#### Supported Parameters and Pad Capabilities

The `mtl_st30p_tx` plugin supports the following pad capabilities:

- **Formats**: `S16LE`, `S24LE`, `S32LE`
- **Sample Rate Range**: 44100, 48000, 96000
- **Channels Range**: 1 to 8

**Arguments**
| Property Name       | Type   | Description                                           | Range                   | Default Value |
|---------------------|--------|-------------------------------------------------------|-------------------------|---------------|
| retry               | uint   | Number of times the MTL will try to get a frame.      | 0 to G_MAXUINT          | 10            |
| tx-samplerate       | uint   | Sample rate of the audio.                             | [gst_mtl_supported_audio_sampling](#supported-audio-sampling-rates-gst_mtl_supported_audio_sampling) | 0 |
| tx-channels         | uint   | Number of audio channels.                             | 1 to 8                  | 2             |

#### Example GStreamer Pipeline for Transmission with s16LE format

To run the `mtl_st30p_tx` plugin, you need to setup metadata (Here we are using pipeline capabilities).
Instead of using input video we opted for build-in GStreamer audio files generator.

```shell
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_T="pci_address_of_the_device"

# Audio pipeline with 48kHz sample rate on port 30000
gst-launch-1.0 audiotestsrc ! \
audio/x-raw,format=S16LE,rate=48000,channels=2 ! \
mtl_st30p_tx tx-queues=4 rx-queues=0 udp-port=30000 payload-type=113 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_T \
--gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

### Running the SMPTE ST 2110-30 Receiver Plugin `mtl_st30p_rx`

#### Supported Parameters and Pad Capabilities

The `mtl_st30p_rx` plugin supports the following pad capabilities:

- **Formats**: `S8`, `S16LE`, `S24LE`
- **Channels Range**: 1 to 2
- **Sample Rate Range**: 44100, 48000, 96000

**Arguments**
| Property Name       | Type    | Description                                           | Range                   | Default Value |
|---------------------|---------|-------------------------------------------------------|-------------------------|---------------|
| rx-framebuff-num    | uint    | Number of framebuffers to be used for transmission.   | 0 to G_MAXUINT          | 3             |
| rx-channel          | uint    | Audio channel number.                                 | 0 to G_MAXUINT          | 2             |
| rx-sampling         | uint    | Audio sampling rate.                                  | [gst_mtl_supported_audio_sampling](#supported-audio-sampling-rates-gst_mtl_supported_audio_sampling) | 48000         |
| rx-audio-format     | string  | Audio format type.                                    | `S8`, `S16LE`, `S24LE`  | `S16LE`       |

#### Preparing Output Path

In our pipelines, we will use the `$OUTPUT` variable to hold the path to the audio file.

```shell
export OUTPUT="path_to_the_file_we_want_to_save"
```

#### Example GStreamer pipeline for reception

To run the `mtl_st30p_rx` audio plugin use the command below:

> **Warning**: To receive data, ensure that a transmission is running with the same parameters on the `239.168.75.30` address.

```shell
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_R="pci_address_of_the_device"

# Set the output path
export OUTPUT="path_to_the_file_we_want_to_save"

# Run the receiver pipeline
gst-launch-1.0 -v mtl_st30p_rx rx-queues=4 udp-port=30000 payload-type=111 dev-ip="192.168.96.2" ip="239.168.75.30" dev-port=$VFIO_PORT_R rx-audio-format=PCM24 rx-channel=2 rx-sampling=48000 ! \
filesink location=$OUTPUT --gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

### SMPT SMPTE ST 2110-40 Ancillary Data Plugins

Ancillary data plugins for MTL that are able to send and receive synchronous ancillary data via the MTL API.

### Running the SMPTE ST 2110-40 Transmission Plugin `mtl_st40p_tx`

#### Supported Parameters and Pad Capabilities

The `mtl_st40p_tx` plugin supports all pad capabilities (the data is not checked):

- **Capabilities**: Any (GST_STATIC_CAPS_ANY)

**Arguments**
| Property Name       | Type   | Description                                           | Range                   | Default Value |
|---------------------|--------|-------------------------------------------------------|-------------------------|---------------|
| retry               | uint   | Number of times the MTL will try to get a frame.      | 0 to G_MAXUINT          | 10            |
| tx-framebuff-cnt    | uint   | Number of framebuffers to be used for transmission.   | 0 to G_MAXUINT          | 3             |

#### Example GStreamer Pipeline for Transmission

To run the `mtl_st40p_tx` plugin, use the following command:

```shell
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_T="pci_address_of_the_device"

# Ancillary data pipeline on port 40000
gst-launch-1.0 -v mtl_st40p_tx tx-queues=4 rx-queues=0 udp-port=40000 payload-type=113 dev-ip="192.168.96.3" ip="239.168.75.30" dev-port=$VFIO_PORT_T retry=10 tx-framebuff-cnt=3 --gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

> **Warning**: To transmit ancillary data, ensure that a receiver is running with the same parameters on the `239.168.75.30` address.

This command sets up the transmission pipeline with the specified parameters and sends the ancillary data using the `mtl_st40p_tx` plugin.


### SMPTE ST 2110-40 Ancillary Data Plugins

Ancillary data plugins for MTL that are able to send and receive synchronous ancillary data via the MTL pipeline API.

### Running the SMPTE ST 2110-40 Receiver Plugin `mtl_st40p_rx`

#### Supported Parameters and Pad Capabilities

The `mtl_st40p_rx` plugin supports all pad capabilities (the data is not checked):

- **Capabilities**: Any (GST_STATIC_CAPS_ANY)

**Arguments**
| Property Name       | Type   | Description                                           | Range                       | Default Value |
|---------------------|--------|-------------------------------------------------------|-----------------------------|---------------|
| buffer-size         | uint   | Size of the buffer used for receiving data            | 0 to G_MAXUINT (power of 2) | 1024          |
| timeout             | uint   | Timeout in seconds for getting mbuf                   | 0 to G_MAXUINT              | 10            |

#### Example GStreamer Pipeline for Reception

To run the `mtl_st40p_rx` plugin, use the following command:
> **Warning**: To receive ancillary data, ensure that a transmission is running on the `239.168.75.30` address.

```shell
# If you don't know how to find the VFIO PCI address of your device
# refer to Media-Transport-Library/doc/run.md
export VFIO_PORT_R="pci_address_of_the_device"

# Set the output path
export OUTPUT="path_to_the_file_we_want_to_save"

# Run the receiver pipeline
gst-launch-1.0 -v mtl_st40p_rx rx-queues=4 udp-port=40000 payload-type=113 dev-ip="$IP_PORT_R" ip="$IP_PORT_T" timeout=61 dev-port=$VFIO_PORT_R ! filesink location=$OUTPUT --gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

