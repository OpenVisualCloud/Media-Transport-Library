extern "C" {
#include "gpu.h"
}

#include <gtest/gtest.h>

#include "fff.h"

DEFINE_FFF_GLOBALS;

// Mock level-zero API functions
FAKE_VALUE_FUNC(ze_result_t, zeInit, ze_init_flags_t);
FAKE_VALUE_FUNC(ze_result_t, zeDriverGet, uint32_t*, ze_driver_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeContextCreate, ze_driver_handle_t,
                const ze_context_desc_t*, ze_context_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeDeviceGet, ze_driver_handle_t, uint32_t*,
                ze_device_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeDeviceGetProperties, ze_device_handle_t,
                ze_device_properties_t*);
FAKE_VALUE_FUNC(ze_result_t, zeMemAllocShared, ze_context_handle_t,
                const ze_device_mem_alloc_desc_t*, const ze_host_mem_alloc_desc_t*,
                size_t, size_t, ze_device_handle_t, void**);
FAKE_VALUE_FUNC(ze_result_t, zeCommandQueueCreate, ze_context_handle_t,
                ze_device_handle_t, const ze_command_queue_desc_t*,
                ze_command_queue_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeCommandListCreate, ze_context_handle_t, ze_device_handle_t,
                const ze_command_list_desc_t*, ze_command_list_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeCommandListAppendMemoryCopy, ze_command_list_handle_t,
                void*, const void*, size_t, ze_event_handle_t, uint32_t,
                ze_event_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeCommandListAppendMemoryFill, ze_command_list_handle_t,
                void*, const void*, size_t, size_t, ze_event_handle_t, uint32_t,
                ze_event_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeCommandListClose, ze_command_list_handle_t);
FAKE_VALUE_FUNC(ze_result_t, zeCommandQueueExecuteCommandLists, ze_command_queue_handle_t,
                uint32_t, ze_command_list_handle_t*, ze_fence_handle_t);
FAKE_VALUE_FUNC(ze_result_t, zeCommandQueueSynchronize, ze_command_queue_handle_t,
                uint64_t);
FAKE_VALUE_FUNC(ze_result_t, zeCommandListReset, ze_command_list_handle_t);
FAKE_VALUE_FUNC(ze_result_t, zeCommandListDestroy, ze_command_list_handle_t);
FAKE_VALUE_FUNC(ze_result_t, zeCommandQueueDestroy, ze_command_queue_handle_t);
FAKE_VALUE_FUNC(ze_result_t, zeMemFree, ze_context_handle_t, void*);

// Memory allocation mocks
FAKE_VOID_FUNC(free, void*);
FAKE_VALUE_FUNC(void*, calloc, size_t, size_t);

class GpuTest : public testing::Test {
 public:
  void SetUp() {
    RESET_FAKE(zeInit);
    RESET_FAKE(zeDriverGet);
    RESET_FAKE(zeContextCreate);
    RESET_FAKE(zeDeviceGet);
    RESET_FAKE(zeDeviceGetProperties);
    RESET_FAKE(zeMemAllocShared);
    RESET_FAKE(zeCommandQueueCreate);
    RESET_FAKE(zeCommandListCreate);
    RESET_FAKE(zeCommandListAppendMemoryCopy);
    RESET_FAKE(zeCommandListAppendMemoryFill);
    RESET_FAKE(zeCommandListClose);
    RESET_FAKE(zeCommandQueueExecuteCommandLists);
    RESET_FAKE(zeCommandQueueSynchronize);
    RESET_FAKE(zeCommandListReset);
    RESET_FAKE(zeCommandListDestroy);
    RESET_FAKE(zeCommandQueueDestroy);
    RESET_FAKE(zeMemFree);
    RESET_FAKE(calloc);
    RESET_FAKE(free);
    FFF_RESET_HISTORY();
  }
  void VerifyCallCountsAreZero(const std::set<std::string>& exceptions = {}) {
    const std::vector<std::pair<std::string, unsigned int>> fakes = {
        {"zeInit", zeInit_fake.call_count},
        {"zeDriverGet", zeDriverGet_fake.call_count},
        {"zeContextCreate", zeContextCreate_fake.call_count},
        {"zeDeviceGet", zeDeviceGet_fake.call_count},
        {"zeDeviceGetProperties", zeDeviceGetProperties_fake.call_count},
        {"zeMemAllocShared", zeMemAllocShared_fake.call_count},
        {"zeCommandQueueCreate", zeCommandQueueCreate_fake.call_count},
        {"zeCommandListCreate", zeCommandListCreate_fake.call_count},
        {"zeCommandListAppendMemoryCopy", zeCommandListAppendMemoryCopy_fake.call_count},
        {"zeCommandListAppendMemoryFill", zeCommandListAppendMemoryFill_fake.call_count},
        {"zeCommandListClose", zeCommandListClose_fake.call_count},
        {"zeCommandQueueExecuteCommandLists",
         zeCommandQueueExecuteCommandLists_fake.call_count},
        {"zeCommandQueueSynchronize", zeCommandQueueSynchronize_fake.call_count},
        {"zeCommandListReset", zeCommandListReset_fake.call_count},
        {"zeCommandListDestroy", zeCommandListDestroy_fake.call_count},
        {"zeCommandQueueDestroy", zeCommandQueueDestroy_fake.call_count},
        {"zeMemFree", zeMemFree_fake.call_count},
        {"calloc", calloc_fake.call_count},
        {"free", free_fake.call_count},
    };

    for (const auto& [name, call_count] : fakes) {
      if (exceptions.find(name) == exceptions.end()) {
        ASSERT_EQ(call_count, 0) << "Call count for '" << name << "' is not zero.";
      }
    }
  }
};

//
// init_level_zero_lib tests
//

TEST_F(GpuTest, init_level_zero_lib_success_ERROR) {
  zeInit_fake.return_val = ZE_RESULT_ERROR_DEVICE_LOST;

  int result = init_level_zero_lib();
  ASSERT_EQ(zeInit_fake.call_count, 1);
  EXPECT_EQ(result, -1);
  VerifyCallCountsAreZero({"zeInit"});
}

TEST_F(GpuTest, init_level_zero_lib_OK) {
  zeInit_fake.return_val = ZE_RESULT_SUCCESS;

  int result = init_level_zero_lib();
  ASSERT_EQ(1, zeInit_fake.call_count);
  EXPECT_EQ(0, result);
  VerifyCallCountsAreZero({"zeInit"});
}

//
// print_gpu_drivers_and_devices
//

TEST_F(GpuTest, print_gpu_drivers_and_devices_ERROR_init) {
  zeInit_fake.return_val = ZE_RESULT_ERROR_UNKNOWN;

  int result = print_gpu_drivers_and_devices();
  ASSERT_EQ(1, zeInit_fake.call_count);
  EXPECT_EQ(-1, result);
  VerifyCallCountsAreZero({"zeInit"});
}

TEST_F(GpuTest, print_gpu_drivers_and_devices_no_drivers_OK) {
  zeInit_fake.return_val = ZE_RESULT_SUCCESS;
  zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t*) {
    *count = 0;  // Set driversCount to 1
    return ZE_RESULT_SUCCESS;
  };

  int result = print_gpu_drivers_and_devices();
  ASSERT_EQ(1, zeInit_fake.call_count);
  ASSERT_EQ(1, zeDriverGet_fake.call_count);
  EXPECT_EQ(0, result);
  VerifyCallCountsAreZero({"zeInit", "zeDriverGet"});
}

TEST_F(GpuTest, PrintGpuDriversAndDevices_ERROR_CallocDrivers) {
  zeInit_fake.return_val = ZE_RESULT_SUCCESS;
  zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t*) {
    *count = 1;  // Set driversCount to 1
    return ZE_RESULT_SUCCESS;
  };
  calloc_fake.return_val = NULL;  // simulate error with calloc

  int result = print_gpu_drivers_and_devices();
  EXPECT_EQ(zeInit_fake.call_count, 1);
  EXPECT_EQ(zeDriverGet_fake.call_count, 1);
  EXPECT_EQ(calloc_fake.call_count, 1);
  EXPECT_EQ(result, -ENOMEM);
  VerifyCallCountsAreZero({"zeInit", "zeDriverGet", "calloc"});
}

TEST_F(GpuTest, TestPrintGpuDriversAndDevices_OK) {
  zeInit_fake.return_val = ZE_RESULT_SUCCESS;
  zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t* handle) {
    *count = 1;  // Set driversCount to 1
    return ZE_RESULT_SUCCESS;
  };
  ze_driver_handle_t mock_drivers[1] = {
      reinterpret_cast<ze_driver_handle_t>(1)};  // mock 1 driver
  calloc_fake.return_val = (void*)mock_drivers;
  zeContextCreate_fake.return_val = ZE_RESULT_SUCCESS;
  zeDeviceGet_fake.custom_fake = [](ze_driver_handle_t driver, uint32_t* count,
                                    ze_device_handle_t* devices) {
    *count = 1;  // return 1 device
    return ZE_RESULT_SUCCESS;
  };
  ze_device_handle_t mock_devices[1] = {
      reinterpret_cast<ze_device_handle_t>(1)};  // mock 1 device
  calloc_fake.return_val = (void*)mock_devices;
  zeDeviceGetProperties_fake.custom_fake = [](ze_device_handle_t device,
                                              ze_device_properties_t* properties) {
    strcpy(properties->name, "Test Device");
    properties->type = ZE_DEVICE_TYPE_GPU;
    properties->vendorId = 0x1234;
    properties->deviceId = 5678;
    return ZE_RESULT_SUCCESS;
  };

  // Capture output
  testing::internal::CaptureStdout();

  // Check calls
  int result = print_gpu_drivers_and_devices();
  EXPECT_EQ(zeInit_fake.call_count, 1);
  EXPECT_EQ(zeDriverGet_fake.call_count, 2);  // First for count, second for handles
  EXPECT_EQ(zeDeviceGet_fake.call_count, 2);  // First for count, second for handles
  EXPECT_EQ(zeContextCreate_fake.call_count, 1);
  EXPECT_EQ(zeDeviceGetProperties_fake.call_count, 1);
  EXPECT_EQ(calloc_fake.call_count, 2);  // First for device, second for driver
  EXPECT_EQ(free_fake.call_count, 2);    // First for device, second for driver
  EXPECT_EQ(result, 0);
  auto output = testing::internal::GetCapturedStdout();

  // Check output
  auto expectedOutput =
      "Drivers count: 1\n"
      "Driver: 0: Device: 0: Name: Test Device, Type: 1, VendorID: 1234, DeviceID: "
      "5678\n";
  EXPECT_EQ(output, expectedOutput);
  VerifyCallCountsAreZero({"zeInit", "zeDriverGet", "zeDeviceGet", "zeContextCreate",
                           "zeDeviceGetProperties", "calloc", "free"});
}

//
// init_gpu_device tests
//

TEST_F(GpuTest, InitGpuDevice_ERROR_ContextAlreadyInitialized) {
  GpuContext ctx = {.initialized = 1};
  int result = init_gpu_device(&ctx, 0, 0);
  EXPECT_EQ(result, -EINVAL);
  VerifyCallCountsAreZero();
}

TEST_F(GpuTest, InitGpuDevice_ERROR_InvalidDriverIndex) {
  GpuContext ctx = {};
  zeInit_fake.return_val = ZE_RESULT_SUCCESS;
  zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t*) {
    *count = 1;
    return ZE_RESULT_SUCCESS;
  };

  int result = init_gpu_device(&ctx, 1, 0);  // Invalid index
  EXPECT_EQ(result, -EINVAL);
  EXPECT_EQ(ctx.initialized, 0);
  EXPECT_EQ(zeInit_fake.call_count, 1);
  EXPECT_EQ(zeDriverGet_fake.call_count, 1);
  VerifyCallCountsAreZero({"zeInit", "zeDriverGet"});
}

TEST_F(GpuTest, InitGpuDevice_ERROR_FailToCreateContext) {
  GpuContext ctx = {};
  zeInit_fake.return_val = ZE_RESULT_SUCCESS;
  zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t* handle) {
    *count = 1;
    return ZE_RESULT_SUCCESS;
  };
  zeDeviceGet_fake.custom_fake = [](ze_driver_handle_t, uint32_t* count,
                                    ze_device_handle_t*) {
    *count = 1;
    return ZE_RESULT_SUCCESS;
  };
  ze_driver_handle_t mock_drivers[1] = {
      reinterpret_cast<ze_driver_handle_t>(1)};  // mock 1 driver
  calloc_fake.return_val = (void*)mock_drivers;
  zeContextCreate_fake.return_val = ZE_RESULT_ERROR_INVALID_ARGUMENT;
  int result = init_gpu_device(&ctx, 0, 0);
  EXPECT_EQ(result, -1);
  EXPECT_EQ(ctx.initialized, 0);
  EXPECT_EQ(zeInit_fake.call_count, 1);
  EXPECT_EQ(zeDriverGet_fake.call_count, 2);
  EXPECT_EQ(zeContextCreate_fake.call_count, 1);
  EXPECT_EQ(calloc_fake.call_count, 1);
  // Exclude free. Free is covered with free_gpu_context(&ctx) test
  VerifyCallCountsAreZero({"zeInit", "zeDriverGet", "zeContextCreate", "calloc", "free"});
  EXPECT_GE(free_fake.call_count, 1);
}

TEST_F(GpuTest, InitGpuDevice_ERROR_InvalidDeviceIndex) {
  GpuContext ctx = {};
  zeInit_fake.return_val = ZE_RESULT_SUCCESS;
  zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t* handle) {
    *count = 1;
    return ZE_RESULT_SUCCESS;
  };
  zeDeviceGet_fake.custom_fake = [](ze_driver_handle_t, uint32_t* count,
                                    ze_device_handle_t*) {
    *count = 1;
    return ZE_RESULT_SUCCESS;
  };
  zeContextCreate_fake.return_val = ZE_RESULT_SUCCESS;
  ze_driver_handle_t mock_drivers[1] = {
      reinterpret_cast<ze_driver_handle_t>(1)};  // mock 1 driver
  calloc_fake.return_val = (void*)mock_drivers;

  int result = init_gpu_device(&ctx, 0, 1);  // Invalid device index
  EXPECT_EQ(result, -EINVAL);
  EXPECT_EQ(ctx.initialized, 0);
  EXPECT_EQ(zeInit_fake.call_count, 1);
  EXPECT_EQ(zeDriverGet_fake.call_count, 2);
  EXPECT_EQ(zeContextCreate_fake.call_count, 1);
  EXPECT_EQ(zeDeviceGet_fake.call_count, 1);
  EXPECT_EQ(calloc_fake.call_count, 1);
  // Exclude free. Free is covered with free_gpu_context(&ctx) test
  VerifyCallCountsAreZero(
      {"zeInit", "zeDriverGet", "zeContextCreate", "zeDeviceGet", "calloc", "free"});
}

TEST_F(GpuTest, InitGpuDevice_OK) {
  GpuContext ctx = {};
  zeInit_fake.return_val = ZE_RESULT_SUCCESS;
  zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t* handle) {
    *count = 1;  // Set driversCount to 1
    return ZE_RESULT_SUCCESS;
  };
  ze_driver_handle_t mock_drivers[1] = {
      reinterpret_cast<ze_driver_handle_t>(1)};  // mock 1 driver
  calloc_fake.return_val = (void*)mock_drivers;
  zeContextCreate_fake.return_val = ZE_RESULT_SUCCESS;
  zeDeviceGet_fake.custom_fake = [](ze_driver_handle_t driver, uint32_t* count,
                                    ze_device_handle_t* devices) {
    *count = 1;  // return 1 device
    return ZE_RESULT_SUCCESS;
  };
  ze_device_handle_t mock_devices[1] = {
      reinterpret_cast<ze_device_handle_t>(1)};  // mock 1 device
  calloc_fake.return_val = (void*)mock_devices;
  zeDeviceGetProperties_fake.custom_fake = [](ze_device_handle_t device,
                                              ze_device_properties_t* properties) {
    strcpy(properties->name, "Test Device");
    properties->type = ZE_DEVICE_TYPE_GPU;
    properties->vendorId = 0x1234;
    properties->deviceId = 5678;
    return ZE_RESULT_SUCCESS;
  };
  zeCommandQueueCreate_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandListCreate_fake.return_val = ZE_RESULT_SUCCESS;

  int result = init_gpu_device(&ctx, 0, 0);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(ctx.initialized, 1);
  EXPECT_EQ(zeInit_fake.call_count, 1);
  EXPECT_EQ(zeDriverGet_fake.call_count, 2);  // First for count, second for handles
  EXPECT_EQ(zeDeviceGet_fake.call_count, 2);  // First for count, second for handles
  EXPECT_EQ(zeContextCreate_fake.call_count, 1);
  EXPECT_EQ(zeDeviceGetProperties_fake.call_count, 1);
  EXPECT_EQ(zeCommandListCreate_fake.call_count, 1);
  EXPECT_EQ(zeCommandQueueCreate_fake.call_count, 1);
  EXPECT_EQ(calloc_fake.call_count, 2);  // First for device, second for driver
  // Exclude free. Free is covered with free_gpu_context(&ctx) test
  VerifyCallCountsAreZero({"zeInit", "zeDriverGet", "zeContextCreate", "zeDeviceGet",
                           "zeDeviceGetProperties", "zeCommandQueueCreate",
                           "zeCommandListCreate", "calloc", "free"});
}

//
// gpu_allocate_shared_buffer tests
//

TEST_F(GpuTest, GpuAllocateSharedBuffer_ERROR_UninitializedContext) {
  GpuContext ctx = {};  // Not initialized
  int result = gpu_allocate_shared_buffer(&ctx, NULL, 1024);
  EXPECT_EQ(result, -1);
  VerifyCallCountsAreZero();
}

TEST_F(GpuTest, GpuAllocateSharedBuffer_ERROR_AllocationFailed) {
  GpuContext ctx = {.initialized = 1,
                    .deviceContext = reinterpret_cast<ze_context_handle_t>(1)};
  zeMemAllocShared_fake.return_val = ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
  int result = gpu_allocate_shared_buffer(&ctx, NULL, 1024);
  EXPECT_EQ(result, -1);
  EXPECT_EQ(zeMemAllocShared_fake.call_count, 1);
  VerifyCallCountsAreZero({"zeMemAllocShared"});
}

TEST_F(GpuTest, GpuAllocateSharedBuffer_OK) {
  GpuContext ctx = {.initialized = 1,
                    .deviceContext = reinterpret_cast<ze_context_handle_t>(1)};
  void* buf = NULL;
  zeMemAllocShared_fake.custom_fake = [](ze_context_handle_t,
                                         const ze_device_mem_alloc_desc_t*,
                                         const ze_host_mem_alloc_desc_t*, size_t, size_t,
                                         ze_device_handle_t, void** buffer) {
    static int dummyValue = 42;  // Example value to write
    *buffer = &dummyValue;
    return ZE_RESULT_SUCCESS;
  };
  int result = gpu_allocate_shared_buffer(&ctx, &buf, 1024);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(zeMemAllocShared_fake.call_count, 1);
  EXPECT_EQ(*static_cast<int*>(buf), 42);
  VerifyCallCountsAreZero({"zeMemAllocShared"});
}

//
// gpu_memcpy tests
//

TEST_F(GpuTest, GpuMemcpy_ERROR_UninitializedContext) {
  GpuContext ctx = {};  // Not initialized
  int result = gpu_memcpy(&ctx, (void*)1, (void*)2, 1024);
  EXPECT_EQ(result, -1);
  VerifyCallCountsAreZero();
}

TEST_F(GpuTest, GpuMemcpy_ERROR_CommandListAppendMemoryCopy) {
  GpuContext ctx = {.initialized = 1,
                    .deviceContext = reinterpret_cast<ze_context_handle_t>(1)};
  zeCommandListAppendMemoryCopy_fake.return_val = ZE_RESULT_ERROR_UNKNOWN;
  int result = gpu_memcpy(&ctx, (void*)1, (void*)2, 1024);
  EXPECT_EQ(result, -1);
  EXPECT_EQ(zeCommandListAppendMemoryCopy_fake.call_count, 1);
  VerifyCallCountsAreZero({"zeCommandListAppendMemoryCopy"});
}

TEST_F(GpuTest, GpuMemcpy_OK) {
  GpuContext ctx = {.initialized = 1,
                    .deviceContext = reinterpret_cast<ze_context_handle_t>(1)};
  zeCommandListAppendMemoryCopy_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandListClose_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandQueueExecuteCommandLists_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandQueueSynchronize_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandListReset_fake.return_val = ZE_RESULT_SUCCESS;
  int result = gpu_memcpy(&ctx, (void*)1, (void*)2, 1024);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(zeCommandListAppendMemoryCopy_fake.call_count, 1);
  EXPECT_EQ(zeCommandListClose_fake.call_count, 1);
  EXPECT_EQ(zeCommandQueueExecuteCommandLists_fake.call_count, 1);
  EXPECT_EQ(zeCommandQueueSynchronize_fake.call_count, 1);
  EXPECT_EQ(zeCommandListReset_fake.call_count, 1);
  VerifyCallCountsAreZero({"zeCommandListAppendMemoryCopy", "zeCommandListClose",
                           "zeCommandQueueExecuteCommandLists",
                           "zeCommandQueueSynchronize", "zeCommandListReset"});
}

//
// gpu_memset tests
//

TEST_F(GpuTest, GpuMemset_ERROR_UninitializedContext) {
  GpuContext ctx = {};  // Not initialized
  int result = gpu_memset(&ctx, (void*)1, 0, 1024);
  EXPECT_EQ(result, -1);
  VerifyCallCountsAreZero();
}

TEST_F(GpuTest, GpuMemset_ERROR_CommandListAppendMemoryFill) {
  GpuContext ctx = {.initialized = 1,
                    .deviceContext = reinterpret_cast<ze_context_handle_t>(1)};
  zeCommandListAppendMemoryFill_fake.return_val = ZE_RESULT_ERROR_UNKNOWN;
  int result = gpu_memset(&ctx, (void*)1, 0, 1024);
  EXPECT_EQ(result, -1);
  EXPECT_EQ(zeCommandListAppendMemoryFill_fake.call_count, 1);
  VerifyCallCountsAreZero({"zeCommandListAppendMemoryFill"});
}

TEST_F(GpuTest, GpuMemset_OK) {
  GpuContext ctx = {.initialized = 1,
                    .deviceContext = reinterpret_cast<ze_context_handle_t>(1)};
  zeCommandListAppendMemoryFill_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandListClose_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandQueueExecuteCommandLists_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandQueueSynchronize_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandListReset_fake.return_val = ZE_RESULT_SUCCESS;
  int result = gpu_memset(&ctx, (void*)1, 0, 1024);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(zeCommandListAppendMemoryFill_fake.call_count, 1);
  EXPECT_EQ(zeCommandListClose_fake.call_count, 1);
  EXPECT_EQ(zeCommandQueueExecuteCommandLists_fake.call_count, 1);
  EXPECT_EQ(zeCommandQueueSynchronize_fake.call_count, 1);
  EXPECT_EQ(zeCommandListReset_fake.call_count, 1);
  VerifyCallCountsAreZero({"zeCommandListAppendMemoryFill", "zeCommandListClose",
                           "zeCommandQueueExecuteCommandLists",
                           "zeCommandQueueSynchronize", "zeCommandListReset"});
}

//
// free_gpu_context tests
//

TEST_F(GpuTest, FreeGpuContext_OK_NullContext) {
  int result = free_gpu_context(NULL);
  EXPECT_EQ(result, 0);
  VerifyCallCountsAreZero();
}

TEST_F(GpuTest, FreeGpuContext_OK) {
  GpuContext ctx = {
      .initialized = 1,
      .deviceCommandQueue = reinterpret_cast<ze_command_queue_handle_t>(1),
      .deviceCommandList = reinterpret_cast<ze_command_list_handle_t>(1),
  };

  zeCommandListDestroy_fake.return_val = ZE_RESULT_SUCCESS;
  zeCommandQueueDestroy_fake.return_val = ZE_RESULT_SUCCESS;
  int result = free_gpu_context(&ctx);
  EXPECT_EQ(result, 0);
  EXPECT_EQ(ctx.initialized, 0);
  EXPECT_EQ(ctx.deviceCommandQueue, nullptr);
  EXPECT_EQ(ctx.deviceCommandList, nullptr);
  EXPECT_EQ(zeCommandListDestroy_fake.call_count, 1);
  EXPECT_EQ(zeCommandQueueDestroy_fake.call_count, 1);
  EXPECT_EQ(free_fake.call_count, 2);
  VerifyCallCountsAreZero({"zeCommandListDestroy", "zeCommandQueueDestroy", "free"});
}