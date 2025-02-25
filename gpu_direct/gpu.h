/* SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation
 */

#ifndef GPU
#define GPU

#include <errno.h>
#include <level_zero/ze_api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct GpuContext {
  uint32_t driverCount;
  uint32_t deviceCount;

  // all drivers and devices
  ze_driver_handle_t* drivers;
  ze_device_handle_t* devices;

  // check if context initialized
  bool initialized;

  // handlers for the current device and drivers
  int currentDriverIndex;
  int currentDeviceIndex;

  // level zero api structs
  ze_driver_handle_t driverHandle;
  ze_context_handle_t deviceContext;
  ze_device_handle_t deviceHandler;
  ze_device_properties_t deviceProperties;
  ze_command_queue_handle_t deviceCommandQueue;
  ze_command_list_handle_t deviceCommandList;
} GpuContext;

int init_level_zero_lib();
int print_gpu_drivers_and_devices();
int init_gpu_device(GpuContext* ctx, unsigned driverIndex, unsigned deviceIndex);
int gpu_allocate_shared_buffer(GpuContext* ctx, void** buf, size_t size);
int gpu_allocate_device_buffer(GpuContext* ctx, void** buf, size_t size);
int gpu_memcpy(GpuContext* ctx, void* dst, const void* src, size_t sz);
int gpu_memset(GpuContext* ctx, void* dst, char byte, size_t sz);
void gpu_free_buf(GpuContext* ctx, void* buf);
int free_gpu_context(GpuContext* ctx);

#endif /* GPU */
