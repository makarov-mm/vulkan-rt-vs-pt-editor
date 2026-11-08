#include "vk_utils.h"

#include <format>
#include <stdexcept>
#include <string_view>

namespace {
std::string_view resultName(VkResult r) noexcept {
    switch (r) {
    case VK_ERROR_OUT_OF_HOST_MEMORY:    return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:  return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:           return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:     return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:     return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:   return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:   return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:      return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:  return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR:      return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:       return "VK_ERROR_OUT_OF_DATE_KHR";
    default:                             return {};
    }
}
} // namespace

void vkCheck(VkResult r, const char* what, std::source_location loc) {
    if (r == VK_SUCCESS) return;
    const std::string_view name = resultName(r);
    const std::string result = name.empty() ? std::format("VkResult {}", static_cast<int>(r)) : std::string(name);
    throw std::runtime_error(what
        ? std::format("Vulkan call failed ({}): {}", result, what)
        : std::format("Vulkan call failed ({}) in {} ({}:{})", result, loc.function_name(), loc.file_name(), loc.line()));
}

Barriers& Barriers::image(VkImage img, VkImageLayout oldLayout, VkImageLayout newLayout, Access src, Access dst) {
    images_.push_back({
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = src.stage,
        .srcAccessMask       = src.access,
        .dstStageMask        = dst.stage,
        .dstAccessMask       = dst.access,
        .oldLayout           = oldLayout,
        .newLayout           = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = img,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    });
    return *this;
}

Barriers& Barriers::memory(Access src, Access dst) {
    memory_.push_back({
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = src.stage,
        .srcAccessMask = src.access,
        .dstStageMask  = dst.stage,
        .dstAccessMask = dst.access,
    });
    return *this;
}

void Barriers::record(VkCommandBuffer c) const {
    const VkDependencyInfo dep{
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount      = static_cast<uint32_t>(memory_.size()),
        .pMemoryBarriers         = memory_.data(),
        .imageMemoryBarrierCount = static_cast<uint32_t>(images_.size()),
        .pImageMemoryBarriers    = images_.data(),
    };
    vkCmdPipelineBarrier2(c, &dep);
}
