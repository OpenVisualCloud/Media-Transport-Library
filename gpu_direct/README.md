# GPU Direct Library

## General Info

This library provides a wrapper for Level Zero API to init GPU and provide functions to allocate shared or device memory.

## Build

Use `meson` to build the project

## How to use it

1) Use `print_gpu_drivers_and_devices` to list drivers and devices index.
2) Create `GpuContext` and use `init_gpu_device` to init gpu context.
3) Allocate memory with  `gpu_allocate_device_buffer` or `gpu_allocate_shared_buffer`.
4) Use `gpu_memcpy` and `gpu_memset` for memcpy and memset operations.
5) Free memory space with `gpu_free_buf` function.
6) Free gpu context with `free_gpu_context`.

## Build MTL GPU-Direct Library

To Build MTL with GPU Direct Library please refer to [doc file](../doc/gpu.md).

## Unit tests

The library contains unit tests.
To run the tests use:

```bash
./run_tests.sh
```

## Links

- [Level Zero Intro](https://www.intel.com/content/www/us/en/developer/articles/technical/using-oneapi-level-zero-interface.html)