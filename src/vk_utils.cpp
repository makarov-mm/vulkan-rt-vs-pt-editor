#include "vk_utils.h"

#include <cstdio>
#include <stdexcept>

void vkCheck(VkResult r, const char* what)
{
    if (r != VK_SUCCESS) 
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Vulkan call failed (%d): %s", static_cast<int>(r), what);
        throw std::runtime_error(buf);
    }
}
