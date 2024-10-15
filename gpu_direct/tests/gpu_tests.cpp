extern "C" {
  #include "gpu.h"
}

#include <gtest/gtest.h>
#include "fff.h"

DEFINE_FFF_GLOBALS;


// Mock level-zero API functions
FAKE_VALUE_FUNC(ze_result_t, zeInit, ze_init_flags_t);
FAKE_VALUE_FUNC(ze_result_t, zeDriverGet, uint32_t*, ze_driver_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeContextCreate, ze_driver_handle_t, const ze_context_desc_t*, ze_context_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeDeviceGet, ze_driver_handle_t, uint32_t*, ze_device_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeDeviceGetProperties, ze_device_handle_t, ze_device_properties_t*);
FAKE_VALUE_FUNC(ze_result_t, zeMemAllocShared, ze_context_handle_t, const ze_device_mem_alloc_desc_t*, const ze_host_mem_alloc_desc_t*, size_t, size_t, ze_device_handle_t, void**);
FAKE_VALUE_FUNC(ze_result_t, zeCommandQueueCreate, ze_context_handle_t, ze_device_handle_t, const ze_command_queue_desc_t*, ze_command_queue_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeCommandListCreate, ze_context_handle_t, ze_device_handle_t, const ze_command_list_desc_t*, ze_command_list_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeCommandListAppendMemoryCopy, ze_command_list_handle_t, void*, const void*, size_t, ze_event_handle_t, uint32_t, ze_event_handle_t*);
FAKE_VALUE_FUNC(ze_result_t, zeCommandListClose, ze_command_list_handle_t);
FAKE_VALUE_FUNC(ze_result_t, zeCommandQueueExecuteCommandLists, ze_command_queue_handle_t, uint32_t, ze_command_list_handle_t*, ze_fence_handle_t);
FAKE_VALUE_FUNC(ze_result_t, zeCommandQueueSynchronize, ze_command_queue_handle_t, uint64_t);
FAKE_VALUE_FUNC(ze_result_t, zeMemFree, ze_context_handle_t, void*);

// Memory allocation mocks
FAKE_VALUE_FUNC(void*, calloc, size_t, size_t);

class GpuTest : public testing::Test
{
public:
    void SetUp()
    {
        RESET_FAKE(zeInit);
        RESET_FAKE(zeDriverGet);
        RESET_FAKE(zeContextCreate);
        RESET_FAKE(calloc);
        FFF_RESET_HISTORY();
    }
};

//
// init_level_zero_lib tests
// 
TEST_F(GpuTest, init_level_zero_lib_success_ERROR) {
    zeInit_fake.return_val = ZE_RESULT_ERROR_DEVICE_LOST;

    int result = init_level_zero_lib();
    ASSERT_EQ(1, zeInit_fake.call_count);
    EXPECT_EQ(-1, result);
}

TEST_F(GpuTest, init_level_zero_lib_OK) {
    zeInit_fake.return_val = ZE_RESULT_SUCCESS;

    int result = init_level_zero_lib();
    ASSERT_EQ(1, zeInit_fake.call_count);
    EXPECT_EQ(0, result);
}

//
// print_gpu_drivers_and_devices
//
TEST_F(GpuTest, print_gpu_drivers_and_devices_ERROR_init) {
    zeInit_fake.return_val = ZE_RESULT_ERROR_UNKNOWN;

    int result = print_gpu_drivers_and_devices();
    ASSERT_EQ(1, zeInit_fake.call_count);
    EXPECT_EQ(-1, result);
}

TEST_F(GpuTest, print_gpu_drivers_and_devices_no_drivers_OK) {
    zeInit_fake.return_val = ZE_RESULT_SUCCESS;
    zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t*) {
        *count = 0; // Set driversCount to 1
        return ZE_RESULT_SUCCESS;
    };

    int result = print_gpu_drivers_and_devices();
    ASSERT_EQ(1, zeInit_fake.call_count);
    ASSERT_EQ(1, zeDriverGet_fake.call_count);
    EXPECT_EQ(0, result);
}

TEST_F(GpuTest, PrintGpuDriversAndDevices_ERROR_CallocDrivers) {
    zeInit_fake.return_val = ZE_RESULT_SUCCESS;
    zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t*) {
        *count = 1; // Set driversCount to 1
        return ZE_RESULT_SUCCESS;
    };
    calloc_fake.return_val = NULL; // simulate error with calloc

    int result = print_gpu_drivers_and_devices();
    EXPECT_EQ(calloc_fake.call_count, 1);
    EXPECT_EQ(result, -ENOMEM);
}


// TEST_F(GpuTest, TestPrintGpuDriversAndDevices_Success) {
//     zeInit_fake.return_val = ZE_RESULT_SUCCESS;
//     zeDriverGet_fake.custom_fake = [](uint32_t* count, ze_driver_handle_t*) {
//         *count = 1; // Set driversCount to 1
//         return ZE_RESULT_SUCCESS;
//     };
//     calloc_fake.return_val = (void*)0x1234; 

//     int result = print_gpu_drivers_and_devices();
//     EXPECT_EQ(zeInit_fake.call_count, 1);
//     EXPECT_EQ(calloc_fake.call_count, 1);
//     EXPECT_EQ(zeDriverGet_fake.call_count, 2); // First for count, second for handles
//     EXPECT_EQ(result, -ENOMEM);
// }