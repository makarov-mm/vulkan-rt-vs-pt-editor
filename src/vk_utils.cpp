#include "vk_utils.h"

#include <format>
#include <stdexcept>

void vkCheck(VkResult r, const char* what, std::source_location loc) {
    if (r == VK_SUCCESS) return;
    throw std::runtime_error(what
        ? std::format("Vulkan call failed ({}): {}", static_cast<int>(r), what)
        : std::format("Vulkan call failed ({}) in {} ({}:{})",
                      static_cast<int>(r), loc.function_name(), loc.file_name(), loc.line()));
}
