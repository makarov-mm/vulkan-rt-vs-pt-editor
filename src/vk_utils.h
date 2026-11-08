#pragma once
#include <concepts>
#include <cstdint>
#include <source_location>
#include <span>
#include <type_traits>
#include <vector>

#include <vulkan/vulkan.h>

#include "cxx26.h"

// A VkBuffer with its dedicated memory. `mapped` is the persistent host
// mapping of host-visible buffers (nullptr for device-only ones).
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

// Throws std::runtime_error naming the result and the failed call (`what`, or
// the calling function when it is omitted).
void vkCheck(VkResult r, const char* what = nullptr,
             std::source_location loc = std::source_location::current());

// Rounds v up to a multiple of a (a must be a power of two).
template <std::unsigned_integral T>
[[nodiscard]] constexpr T alignUp(T v, T a) noexcept {
    return (v + a - 1) & ~(a - 1);
}

// The two-call enumeration idiom: `fn(&count, data)` is called once for the
// count and once for the data. A VkResult it returns is checked.
template <class T, class Fn>
[[nodiscard]] std::vector<T> enumerate(Fn fn, const char* what) {
    auto call = [&](uint32_t* n, T* data) {
        if constexpr (std::is_void_v<std::invoke_result_t<Fn&, uint32_t*, T*>>) {
            fn(n, data);
        } else if (const VkResult r = fn(n, data); r != VK_INCOMPLETE) {
            vkCheck(r, what);
        }
    };
    uint32_t n = 0;
    call(&n, nullptr);
    std::vector<T> v(n);
    call(&n, v.data());
    v.resize(n);
    return v;
}

// -----------------------------------------------------------------------------
//  Synchronization2
// -----------------------------------------------------------------------------
// One side of a dependency: pipeline stages and the memory accesses in them.
struct Access {
    VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        access = VK_ACCESS_2_NONE;
};

// Collects barriers and records them with a single vkCmdPipelineBarrier2.
// Image barriers cover the whole colour image (one mip, one layer).
class Barriers {
public:
    Barriers& image(VkImage img, VkImageLayout oldLayout, VkImageLayout newLayout, Access src, Access dst);
    Barriers& memory(Access src, Access dst);   // global: storage images / buffers without a layout change
    void      record(VkCommandBuffer c) const;

private:
    FixedVector<VkImageMemoryBarrier2, 8> images_;
    FixedVector<VkMemoryBarrier2, 2>      memory_;
};
