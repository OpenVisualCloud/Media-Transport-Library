# GPU

This is an experimental feature

## General Info

The idea to use Lever Zero API to allocation buffers directly in GPU to reduce amount of copy from kernel to user space.
GPU <-> NIC.

This library provides a wrapper for Level Zero to init GPU and provide functions to allocate shared or device memory.

## Build

Use Cmake to build the project

## How to use it

1) Use 'get_devices' to list drivers and devices index.
2) Use 'init_gpu_device' to init gpu context
3) Allocate memory with  'gpu_allocate_device_buffer' or 'gpu_allocate_shared_buffer'
4) Use 'gpu_memcpy' and 'gpu_memset' for memcpy and memset operations
5) Free space with gpu_free_buf.
6) Free gpu context with free_gpu_context.

## Build MTL GPU-Direct Library
Use Meson to build the GPU-Direct library specifically.

``` bash
cd <mtl>/gpu_direct
meson setup build
sudo meson install -C build
# check package installed
pkg-config --libs mtl_gpu_direct

# build the mtl library
./build.sh
```

``` bash
Run TX Sample App
Prepare a file (test.yuv) of 1920x1080 UYVY frames to send. You can refer to run.md for more details.

./build/app/GpuDirectVideoTxMultiSample 192.168.99.110 20000 test.yuv
Run RX Sample App
You need the SDL library to display the received frame.
```

``` bash
./build/app/GpuDirectVideoRxMultiSample 192.168.99.111 192.168.99.110 20000
```


## How to enable it in MTL

Currently, only the ST20P receive frame mode supports VRAM frame allocation.
To enable this feature, use the following flag while initializing the session:  
`ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS`

This setting instructs MTL to allocate frames directly in VRAM.

Additionally, you must initialize the GPU device in your application using this library  
`init_gpu_device` function.


Pass the address of the device with the gpuContext parameter:  
`gpuContext` to the st20p rx flags during session initalization.

**Warning:** Direct memory access functionality is disabled when using this flag. Memory allocated in VRAM cannot be accessed directly using dpdk api.

### Links

- [Level Zero Intro](https://www.intel.com/content/www/us/en/developer/articles/technical/using-oneapi-level-zero-interface.html)