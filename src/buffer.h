#pragma once
#include "platform.h"

// A VkBuffer with its backing memory (all buffers in this project are
// dedicated allocations).
struct Buffer {
    VkBuffer       buf  = VK_NULL_HANDLE;
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    VkDeviceSize   size = 0;
};
