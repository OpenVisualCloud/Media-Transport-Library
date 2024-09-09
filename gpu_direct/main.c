#include "gpu.h"

int main(int argc, char** argv) {
  // 1 context for 1 device
  GpuContext gpu_ctx = {};
  int res = init_gpu_device(&gpu_ctx, 0, 0);
  if (res < 0) {
    return -1;
  }

  // GpuContext gpu_ctx2 = {};
  // int res = init_gpu_device(&gpu_ctx2, 0, 1);
  // if (res < 0) {
  //     return -1;
  // }

  // Malloc GPU
  void* gpuBuf = NULL;
  res = gpu_allocate_device_buffer(&gpu_ctx, &gpuBuf, 1024);
  if (res < 0) {
    return -1;
  }

  // TODO:
  char greetings[] = "Hello World!";
  // Get pixels and put them in the frame buffer
  res = gpu_memcpy(&gpu_ctx, gpuBuf, greetings, sizeof(greetings));
  if (res < 0) {
    return -1;
  }
  // printf("%s", (char*)gpuBuf);

  char str[14];
  printf("%s\n", str);
  gpu_memcpy(&gpu_ctx, str, gpuBuf, 13);
  printf("%s\n", str);

  gpu_free_buf(&gpu_ctx, gpuBuf);

  return 0;
}