/* SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation
 */

#include "gpu.h"

// Macros for error checking
#define ZE_CHECK_ERROR(fn_call)                                                   \
  do {                                                                            \
    ze_result_t status = (fn_call);                                               \
    if (status != ZE_RESULT_SUCCESS) {                                            \
      fprintf(stderr, "Runtime error: %s returned %d at %s:%d", #fn_call, status, \
              __FILE__, __LINE__);                                                \
      return -1;                                                                  \
    }                                                                             \
  } while (0)

#define INIT_CHECK_ERROR(fn_call)                                                  \
  do {                                                                             \
    int init_status = (fn_call);                                                   \
    if (init_status == -1) {                                                       \
      fprintf(stderr, "Initialization error: %s returned %d at %s:%d\n", #fn_call, \
              init_status, __FILE__, __LINE__);                                    \
      return -1;                                                                   \
    }                                                                              \
  } while (0)

#define CTX_CHECK_INIT(ctx)                                                         \
  do {                                                                              \
    if (ctx == NULL || !ctx->initialized) {                                         \
      fprintf(stderr, "Context in not initialized at %s:%d\n", __FILE__, __LINE__); \
      return -1;                                                                    \
    }                                                                               \
  } while (0)

/**
 * @brief Init level zero lib
 * Must be called before calling level zero API.
 * @return int - 0 if successful, < 0 else.
 */
int init_level_zero_lib() {
  ZE_CHECK_ERROR(zeInit(ZE_INIT_FLAG_GPU_ONLY));
  // printf("Level-Zero initialized\n");
  return 0;
}

/**
 * @brief Print drivers and devices indexes
 *
 * @return int - 0 if successful, < 0 else.
 */
int print_gpu_drivers_and_devices() {
  // init level-zero lib
  INIT_CHECK_ERROR(init_level_zero_lib());

  // Get drivers
  uint32_t driversCount = 0;
  ZE_CHECK_ERROR(zeDriverGet(&driversCount, NULL));
  printf("Drivers count: %d\n", driversCount);
  if (driversCount == 0) {
    return 0;
  }

  ze_driver_handle_t* drivers = calloc(driversCount, sizeof(ze_driver_handle_t));
  if (!drivers) {
    fprintf(stderr, "Memory allocation for drivers failed\n");
    return -ENOMEM;
  }
  ZE_CHECK_ERROR(zeDriverGet(&driversCount, drivers));
  for (int i = 0; i < driversCount; i++) {
    ze_context_desc_t ctxtDesc = {ZE_STRUCTURE_TYPE_CONTEXT_DESC, NULL, 0};
    ze_context_handle_t context;
    ze_driver_handle_t driver = drivers[i];
    ZE_CHECK_ERROR(zeContextCreate(driver, &ctxtDesc, &context));

    // Get devices for driver
    uint32_t devicesCount = 0;
    ZE_CHECK_ERROR(zeDeviceGet(driver, &devicesCount, NULL));
    ze_device_handle_t* devices = calloc(devicesCount, sizeof(ze_device_handle_t));
    if (!devices) {
      fprintf(stderr, "Memory allocation for devices failed\n");
      free(drivers);
      return -ENOMEM;
    }
    ZE_CHECK_ERROR(zeDeviceGet(driver, &devicesCount, devices));
    for (int j = 0; j < devicesCount; j++) {
      ze_device_handle_t device = devices[j];
      // Get properties of the device
      ze_device_properties_t deviceProperties;
      ZE_CHECK_ERROR(zeDeviceGetProperties(device, &deviceProperties));
      printf("Driver: %d: Device: %d: Name: %s, Type: %d, VendorID: %x, DeviceID: %d\n",
             i, j, deviceProperties.name, deviceProperties.type,
             deviceProperties.vendorId, deviceProperties.deviceId);
    }
    free(devices);
  }

  free(drivers);
  return 0;
}

/**
 * @brief Init GPU device. Driver index and device index is required.
 * To get indexes, use GetGPUDriversAndDevices().
 * After finishing working with the context calling FreeGpuContext(ctx) is required to
 * free the resources.
 *
 * @param ctx - [in/out] GPU context to init device, context will be updated during
 * initialization.
 * @param driverIndex - [in] driver index.
 * @param deviceIndex - [in] device index.
 * @return int. 0 if successful, < 0 else.
 */
int init_gpu_device(GpuContext* ctx, unsigned driverIndex, unsigned deviceIndex) {
  if (ctx->initialized) {
    fprintf(stderr, "Context is already initialized\n");
    return -EINVAL;
  }

  // Init level-zero lib
  INIT_CHECK_ERROR(init_level_zero_lib());

  // Get drivers count
  ZE_CHECK_ERROR(zeDriverGet(&ctx->driverCount, NULL));
  if (driverIndex >= ctx->driverCount) {
    fprintf(stderr, "Init error: provided driver index is out of range\n");
    return -EINVAL;
  }

  // Allocate and retrieve driver handlers
  ctx->drivers = calloc(ctx->driverCount, sizeof(ze_driver_handle_t));
  if (ctx->drivers == NULL) {
    fprintf(stderr, "Can't allocation memory for drivers handlers\n");
    return -ENOMEM;
  }
  ZE_CHECK_ERROR(zeDriverGet(&ctx->driverCount, ctx->drivers));
  ctx->currentDriverIndex = driverIndex;
  ctx->driverHandle = ctx->drivers[driverIndex];

  // Init context
  ze_context_desc_t ctxDesc = {.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC};
  ZE_CHECK_ERROR(zeContextCreate(ctx->driverHandle, &ctxDesc, &ctx->deviceContext));

  // Get device count
  ZE_CHECK_ERROR(zeDeviceGet(ctx->driverHandle, &ctx->deviceCount, NULL));
  printf("Number of devices: %d\n", ctx->deviceCount);
  if (deviceIndex >= ctx->deviceCount) {
    fprintf(stderr, "Init error: provided device index is out of range\n");
    return -EINVAL;
  }

  // Allocate and retrieve device handlers
  ctx->devices = calloc(ctx->deviceCount, sizeof(ze_device_handle_t));
  if (!ctx->devices) {
    fprintf(stderr, "Can't allocate memory for devices handlers\n");
    free(ctx->drivers);
    return -ENOMEM;
  }
  ZE_CHECK_ERROR(zeDeviceGet(ctx->driverHandle, &ctx->deviceCount, ctx->devices));
  ctx->currentDeviceIndex = deviceIndex;
  ctx->deviceHandler = ctx->devices[deviceIndex];

  // Get properties of the selected device
  ZE_CHECK_ERROR(zeDeviceGetProperties(ctx->deviceHandler, &ctx->deviceProperties));
  printf("Device initialized: Index: %d, Name: %s, Type: %d, VendorID: %x\n", deviceIndex,
         ctx->deviceProperties.name, ctx->deviceProperties.type,
         ctx->deviceProperties.vendorId);

  // Create device command queue
  ze_command_queue_desc_t l0_cq_desc = {
      .stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC,
      .pNext = NULL,
      .ordinal = 0, /* this must be less than device_properties.numAsyncComputeEngines */
      .index = 0,
      .flags = 0,
      .mode = ZE_COMMAND_QUEUE_MODE_DEFAULT,
      .priority = ZE_COMMAND_QUEUE_PRIORITY_NORMAL};
  ZE_CHECK_ERROR(zeCommandQueueCreate(ctx->deviceContext, ctx->deviceHandler, &l0_cq_desc,
                                      &ctx->deviceCommandQueue));

  // Create device command list
  ze_command_list_desc_t l0_cl_desc = {.stype = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC,
                                       .pNext = NULL,
                                       .commandQueueGroupOrdinal = 0,
                                       .flags = 0};
  ZE_CHECK_ERROR(zeCommandListCreate(ctx->deviceContext, ctx->deviceHandler, &l0_cl_desc,
                                     &ctx->deviceCommandList));

  ctx->initialized = 1;
  return 0;
}

/**
 * @brief Allocate shared memory for the device.
 *
 * @param ctx - GPU context of the device.
 * @param buf in/out - buf pointer to be filled
 * @param size
 * @return int. 0 if successful, < 0 else.
 */
int gpu_allocate_shared_buffer(GpuContext* ctx, void** buf, size_t size) {
  // check if ctx is initialized
  CTX_CHECK_INIT(ctx);

  ze_device_mem_alloc_desc_t deviceMemDesc = {
      .stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC,
      .pNext = NULL,
      .flags = 0,
      .ordinal = 0 /* this must be less than count of zeDeviceGetMemoryProperties */
  };
  ze_host_mem_alloc_desc_t hostMemDesc = {
      .stype = ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC, .pNext = NULL, .flags = 0};
  ZE_CHECK_ERROR(zeMemAllocShared(ctx->deviceContext, &deviceMemDesc, &hostMemDesc, size,
                                  16, ctx->deviceHandler, buf));
  printf("shared memory allocated (ptr = %p, size = 0x%lx, device_handle = %p)\n", *buf,
         size, ctx->deviceHandler);

  return 0;
}

/**
 * @brief Allocate device buffer for the device.
 *
 * @param ctx [in].GPU context of the device.
 * @param buf [in/out] - buf pointer to be filled
 * @param size [in]. Buf size
 * @return int. 0 if successful, < 0 else.
 */
int gpu_allocate_device_buffer(GpuContext* ctx, void** buf, size_t size) {
  // check if ctx is initialized
  CTX_CHECK_INIT(ctx);

  ze_device_mem_alloc_desc_t deviceMemDesc = {
      .stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC,
      .pNext = NULL,
      .flags = 0,
      .ordinal = 0 /* this must be less than count of zeDeviceGetMemoryProperties */
  };
  ZE_CHECK_ERROR(zeMemAllocDevice(ctx->deviceContext, &deviceMemDesc, size,
                                  sizeof(unsigned), ctx->deviceHandler, (void**)buf));
  printf("Device memory allocated (ptr = %p, size = 0x%lx, device_handle = %p)\n", (*buf),
         size, ctx->deviceHandler);

  return 0;
}

/**
 * @brief Mem copy
 *
 * @param ctx
 * @param dst
 * @param src
 * @param sz
 * @return int. 0 if successful. -1 if error occurred
 */
int gpu_memcpy(GpuContext* ctx, void* dst, const void* src, size_t sz) {
  // check if ctx is initialized
  CTX_CHECK_INIT(ctx);

  ZE_CHECK_ERROR(
      zeCommandListAppendMemoryCopy(ctx->deviceCommandList, dst, src, sz, NULL, 0, NULL));
  ZE_CHECK_ERROR(zeCommandListClose(ctx->deviceCommandList));
  ZE_CHECK_ERROR(zeCommandQueueExecuteCommandLists(ctx->deviceCommandQueue, 1,
                                                   &ctx->deviceCommandList, NULL));
  ZE_CHECK_ERROR(zeCommandQueueSynchronize(ctx->deviceCommandQueue, UINT32_MAX));
  ZE_CHECK_ERROR(zeCommandListReset(ctx->deviceCommandList));
  printf("gpu_memcpy: src=%p dst=%p sz=%zu\n", src, dst, sz);

  return 0;
}

/**
 * @brief GPU memset
 *
 * @param ctx
 * @param dst
 * @param byte
 * @param sz
 * @return int
 */
int gpu_memset(GpuContext* ctx, void* dst, char byte, size_t sz) {
  // check if ctx is initialized
  CTX_CHECK_INIT(ctx);

  ZE_CHECK_ERROR(zeCommandListAppendMemoryFill(ctx->deviceCommandList, dst, &byte, 1, sz,
                                               NULL, 0, NULL));
  ZE_CHECK_ERROR(zeCommandListClose(ctx->deviceCommandList));
  ZE_CHECK_ERROR(zeCommandQueueExecuteCommandLists(ctx->deviceCommandQueue, 1,
                                                   &ctx->deviceCommandList, NULL));
  ZE_CHECK_ERROR(zeCommandQueueSynchronize(ctx->deviceCommandQueue, UINT32_MAX));
  ZE_CHECK_ERROR(zeCommandListReset(ctx->deviceCommandList));
  printf("gpu_memset\n");

  return 0;
}

void gpu_free_buf(GpuContext* ctx, void* buf) {
  // check if ctx is initialized
  if (ctx == NULL || ctx->deviceContext == NULL) {
    return;
  }

  if (buf != NULL) {
    // Don't need to check
    zeMemFree(ctx->deviceContext, buf);
  }
}

/**
 * @brief Free Gpu Context.
 *
 * @param ctx
 * @return int. 0 if successful, < 0 else.
 */
int free_gpu_context(GpuContext* ctx) {
  if (ctx == NULL) {
    return 0;
  }

  if (ctx->deviceCommandList != NULL) {
    ZE_CHECK_ERROR(zeCommandListDestroy(ctx->deviceCommandList));
    ctx->deviceCommandList = NULL;
  }

  if (ctx->deviceCommandQueue != NULL) {
    ZE_CHECK_ERROR(zeCommandQueueDestroy(ctx->deviceCommandQueue));
    ctx->deviceCommandQueue = NULL;
  }

  free(ctx->devices);
  free(ctx->drivers);
  ctx->initialized = 0;
  return 0;
}
