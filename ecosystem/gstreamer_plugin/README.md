# Gstreamer plugin for MTL

## Building the gstreamer plugins

Before you begin, ensure you have the following installed on your system:  
``` Meson ```


```shell
cd Media-Transport-Library/ecosystem/gstreamer_plugin
./build.sh
```

## Running the pipeline

### General argumetns
In gstreamer plugins there are general arguments that apply to every plugin

| Property Name | Type   | Description                                                                                       | Default Value | Range                    |
|---------------|--------|---------------------------------------------------------------------------------------------------|---------------|--------------------------|
| log-level     | uint   | Set the log level (INFO 1 to CRIT 5).                                                             | 1             | 1 (INFO) TO 5 (CRITICAL) |
| dev-port      | string | DPDK port for synchronous ST 2110 data video transmission, bound to the VFIO DPDK driver.         | NULL          | N/A                      |
| dev-ip        | string | Local IP address that the port will be identified by. This is the address from which ARP responses will be sent. | NULL | N/A                |
| dma-dev       | string | DPDK port for the MTL direct memory functionality.                                                | NULL          | N/A                      |
| port          | string | DPDK device port initialized for the transmission.                                                | NULL          | N/A                      |
| ip            | string | Receiving MTL node IP address.                                                                    | NULL          | N/A                      |
| udp-port      | uint   | Receiving MTL node UDP port.                                                                      | 20000         | 0 to G_MAXUINT           |
| tx-queues     | uint   | Number of TX queues to initialize in DPDK backend.                                                | 16            | 0 to G_MAXUINT           |
| rx-queues     | uint   | Number of RX queues to initialize in DPDK backend.                                                | 16            | 0 to G_MAXUINT           |
| payload-type  | uint   | SMPTE ST 2110 payload type.                                                                       | 112           | 0 to G_MAXUINT           |


### St20 rawvideo plugin

To run the st20 rawvideo plugin you need to pass the path to the plugin to your
gstreamer aplication.

you need to also prepare v210 video

```shell
export INPUT="path_to_the_input_v210_file"

# you can produce it with gst-launch-1.0
gst-launch-1.0 -v videotestsrc pattern=ball ! video/x-raw,width=1920,height=1080,format=v210,framerate=60/1 ! filesink location=$INPUT
```

```shell
# DPDK PCI address bound (see doc/build.md)
export VFIO_PORT_T="pci address of the device"

# Path to the built GStreamer plugin
export GSTREAMER_PLUGINS_PATH="path to the folder with builded plugins"

# video pipeline
gst-launch-1.0 filesrc location=$INPUT ! \
rawvideoparse format=v210 height=1080 width=1920 framerate=60/1 ! \
mtltxsink tx-queues=4 tx-udp-port=20000 tx-payload-type=112 dev-ip="192.168.96.3" tx-ip="239.168.75.30" dev-port=$VFIO_PORT_T \
--gst-plugin-path $GSTREAMER_PLUGINS_PATH


# looping video pipeline
gst-launch-1.0 multifilesrc location=$INPUT loop=true ! \
rawvideoparse format=v210 height=1080 width=1920 framerate=60/1 ! \
mtltxsink tx-queues=4 tx-udp-port=20000 tx-payload-type=112 dev-ip="192.168.96.3" tx-ip="239.168.75.30" dev-port=$VFIO_PORT_T \
--gst-plugin-path $GSTREAMER_PLUGINS_PATH
```

## Supported formats
The mtltxsink element supports the following pad capabilities:

Format:
v210
I422_10LE

Width:
Range: 64 to 16384 pixels

Height:
Range: 64 to 8704 pixels

Framerate:
Supported values: 24, 25, 30, 50, 60, 120
If you use the `fps` property to overwrite the caps, you can use the following framerates:
120 119.88 100 60 59.94 50 30 29.97 25 24 23.98

Framebuffers passed need to be the size of the frame

## Supported parameters for the plugin 

- **dev-port** (Required)
  - **Type:** String
  - **Description:** DPDK device port for synchronous ST 2110-20 uncompressed video transmission, bound to the VFIO DPDK driver.
  - **Default Value:** NULL

- **dev-ip** (Required)
  - **Type:** String
  - **Description:** Local IP address that the port will be identified by. This is the address from which ARP responses will be sent.
  - **Default Value:** NULL

- **tx-ip** (Required)
  - **Type:** String
  - **Description:** Receiving MTL node IP address.
  - **Default Value:** NULL

- **tx-udp-port** (Required)
  - **Type:** Unsigned Integer
  - **Description:** Receiving MTL node UDP port.
  - **Default Value:** 20000
  - **Range:** 0 to G_MAXUINT

- **tx-payload-type** (Required)
  - **Type:** Unsigned Integer
  - **Description:** SMPTE ST 2110 payload type.
  - **Default Value:** 112
  - **Range:** 0 to G_MAXUINT

- **silent**
  - **Type:** Boolean
  - **Description:** Turn on silent mode.
  - **Default Value:** FALSE

- **tx-queues**
  - **Type:** Unsigned Integer
  - **Description:** Number of TX queues to initialize in DPDK backend.
  - **Default Value:** 16
  - **Range:** 0 to G_MAXUINT

- **tx-fps**
  - **Type:** Unsigned Integer
  - **Description:** Framerate of the video.
  - **Default Value:** 0
  - **Range:** 0 to G_MAXUINT

- **tx-framebuff-num**
  - **Type:** Unsigned Integer
  - **Description:** Number of framebuffers to be used for transmission.
  - **Default Value:** 3
  - **Range:** 0 to G_MAXUINT