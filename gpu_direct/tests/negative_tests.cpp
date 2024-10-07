extern "C" {
  #include "gpu.h"
}

#include <gtest/gtest.h>

// Test Cases for gpu_allocate_shared_buffer 
TEST(gpu_allocate_shared_buffer, fail_when_context_is_null) {
    int result = gpu_allocate_shared_buffer(NULL, 0, 0);
    EXPECT_EQ(-1, result);
}

TEST(gpu_allocate_shared_buffer, fail_when_context_is_not_initialized) {
    GpuContext ctx = {};
    int result = gpu_allocate_shared_buffer(&ctx, 0, 0);
    EXPECT_EQ(-1, result);
}

// Test Cases for gpu_allocate_device_buffer 
TEST(gpu_allocate_device_buffer, fail_when_context_is_null) {
    int result = gpu_allocate_device_buffer(NULL, 0, 0);
    EXPECT_EQ(-1, result);
}

TEST(gpu_allocate_device_buffer, fail_when_context_is_not_initialized) {
    GpuContext ctx = {};
    int result = gpu_allocate_device_buffer(&ctx, 0, 0);
    EXPECT_EQ(result, result);
}