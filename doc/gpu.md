# GPU

## General Info

It's possible to create a memory buffer in GPU for the frames in st20 protocol.
This is done by using [gpu direct](../gpu_direct/README.md) library.

Refer to [gpu direct s20 pipeline](../app/sample/gpu_direct) to see an example.

## Build MTL GPU-Direct Library

Use Meson to build the GPU-Direct library specifically.

```bash
cd <mtl>/gpu_direct
meson setup build
sudo meson install -C build

# check package installed
pkg-config --libs mtl_gpu_direct

# build the mtl library
./build.sh
```

Run TX Sample App

Prepare a file (test.yuv) of 1920x1080 UYVY frames to send. You can refer to [run guide](../doc/run.md) for more details.

```bash
./build/app/GpuDirectVideoTxMultiSample 192.168.99.110 20000 test.yuv
```

Run RX Sample App

You need the SDL library to display the received frame.

```bash
./build/app/GpuDirectVideoRxMultiSample 192.168.99.111 192.168.99.110 20000
```

## How to enable it in MTL

Currently, only the ST20P receive frame mode supports VRAM frame allocation.

To enable this feature, use the following flag while initializing the session:  
`ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS`

This setting instructs MTL to allocate frames directly in VRAM.

Additionally, you must initialize the GPU device in your application using gpu direct library by
`init_gpu_device` function.

Pass the address of the device with the gpu_context parameter:  
`gpu_context` to the st20p rx flags during session initialization.

**Warning:** Direct memory access functionality is disabled when using this flag. Memory allocated in VRAM cannot be accessed directly using DPDK API.
