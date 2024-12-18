# Gstreamer plugin for MTL

## Building the gstreamer plugins

```shell
cd Media-Transport-Library/ecosystem/gstreamer_plugin
./build.sh
```

## Running the pipeline

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
