#pragma once
#include <concepts>
#include <source_location>

#include "platform.h"

// A VkBuffer with its dedicated memory. `mapped` is the persistent host
// mapping for host-visible buffers (nullptr for device-only ones).
struct Buffer {
    VkBuffer       buf    = VK_NULL_HANDLE;
    VkDeviceMemory mem    = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
    void*          mapped = nullptr;
};

// A 2D image with its dedicated memory and a full-image view.
struct Image {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory mem   = VK_NULL_HANDLE;
    VkImageView    view  = VK_NULL_HANDLE;
};

// Throws std::runtime_error naming the failed call (or `what`, when given).
void vkCheck(VkResult r, const char* what = nullptr,
             std::source_location loc = std::source_location::current());

// Rounds v up to a multiple of a (a must be a power of two).
template <std::unsigned_integral T>
[[nodiscard]] constexpr T alignUp(T v, T a) noexcept {
    return (v + a - 1) & ~(a - 1);
}
