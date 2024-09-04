#include "gpu.h"

int main(int argc, char** argv) {
  // 1 context for 1 device
  GpuContext gpuCtx = {};
  int res = init_gpu_device(&gpuCtx, 0, 0);
  if (res < 0) {
    return -1;
  }

  // GpuContext gpuCtx2 = {};
  // int res = init_gpu_device(&gpuCtx2, 0, 1);
  // if (res < 0) {
  //     return -1;
  // }

  // Malloc GPU
  void* gpuBuf = NULL;
  res = gpu_allocate_device_buffer(&gpuCtx, &gpuBuf, 1024);
  if (res < 0) {
    return -1;
  }

  // TODO:
  char greetings[] = "Hello World!";
  // Get pixels and put them in the frame buffer
  res = gpu_memcpy(&gpuCtx, gpuBuf, greetings, sizeof(greetings));
  if (res < 0) {
    return -1;
  }
  // printf("%s", (char*)gpuBuf);

  char str[14];
  printf("%s\n", str);
  gpu_memcpy(&gpuCtx, str, gpuBuf, 13);
  printf("%s\n", str);

  gpu_free_buf(&gpuCtx, gpuBuf);

  return 0;
}