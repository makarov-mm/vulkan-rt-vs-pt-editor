#include "presenter.h"

#include <algorithm>
#include <stdexcept>

namespace {
// The acquire semaphore is waited on in the blit stage, and the barrier that
// moves the swapchain image out of UNDEFINED starts from that same stage:
// this forms the dependency chain that keeps the layout transition from
// running before the presentation engine has released the image.
constexpr VkPipelineStageFlags2 BLIT_STAGE = VK_PIPELINE_STAGE_2_BLIT_BIT;

// Everything in the frame, including the final layout transition, happens
// before the render-finished semaphore is signalled.
constexpr VkPipelineStageFlags2 PRESENT_STAGE = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
} // namespace

Presenter::Presenter(VulkanContext& vk, VkExtent2D preferredExtent) : vk_(vk), preferredExtent_(preferredExtent) {
    try {
        const VkDevice dev = vk_.device();
        const VkCommandBufferAllocateInfo ai{
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = vk_.commandPool(),
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        vkCheck(vkAllocateCommandBuffers(dev, &ai, &cmd_), "vkAllocateCommandBuffers");

        const VkSemaphoreCreateInfo sci{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCheck(vkCreateSemaphore(dev, &sci, nullptr, &imageAvailable_), "vkCreateSemaphore");

        const VkFenceCreateInfo fci{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        vkCheck(vkCreateFence(dev, &fci, nullptr, &inFlight_), "vkCreateFence");
    } catch (...) {
        destroy();
        throw;
    }
}

Presenter::~Presenter() {
    destroy();
}

void Presenter::destroy() noexcept {
    const VkDevice dev = vk_.device();
    vkDeviceWaitIdle(dev);
    destroySwapchain();
    if (inFlight_)       vkDestroyFence(dev, inFlight_, nullptr);
    if (imageAvailable_) vkDestroySemaphore(dev, imageAvailable_, nullptr);
    if (cmd_)            vkFreeCommandBuffers(dev, vk_.commandPool(), 1, &cmd_);
    inFlight_       = VK_NULL_HANDLE;
    imageAvailable_ = VK_NULL_HANDLE;
    cmd_            = VK_NULL_HANDLE;
}

void Presenter::destroySwapchain() noexcept {
    const VkDevice dev = vk_.device();
    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(dev, s, nullptr);
    renderFinished_.clear();
    if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    images_.clear();
}

bool Presenter::recreateSwapchain() {
    const VkPhysicalDevice phys    = vk_.physicalDevice();
    const VkSurfaceKHR     surface = vk_.surface();
    const VkDevice         dev     = vk_.device();

    VkSurfaceCapabilitiesKHR caps;
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        throw std::runtime_error("The surface does not support blitting into swapchain images");

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {   // the surface size follows the swapchain
        extent = { std::clamp(preferredExtent_.width,  caps.minImageExtent.width,  caps.maxImageExtent.width),
                   std::clamp(preferredExtent_.height, caps.minImageExtent.height, caps.maxImageExtent.height) };
    }
    if (extent.width == 0 || extent.height == 0) return false;   // minimized

    vkCheck(vkDeviceWaitIdle(dev), "vkDeviceWaitIdle");

    // The shaders write display-referred (already gamma-encoded) values, so
    // prefer a UNORM format; an sRGB format would apply gamma twice.
    const auto formats = enumerate<VkSurfaceFormatKHR>(
        [&](uint32_t* n, VkSurfaceFormatKHR* f) { return vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, n, f); },
        "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (formats.empty()) throw std::runtime_error("The surface reports no formats");
    const auto unorm = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& f) {
        return (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
               f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    format_ = (unorm != formats.end()) ? *unorm : formats.front();
    extent_ = extent;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    const VkSwapchainKHR old = swapchain_;
    const VkSwapchainCreateInfoKHR ci{
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = surface,
        .minImageCount    = imageCount,
        .imageFormat      = format_.format,
        .imageColorSpace  = format_.colorSpace,
        .imageExtent      = extent_,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                          ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode      = VK_PRESENT_MODE_FIFO_KHR,
        .clipped          = VK_TRUE,
        .oldSwapchain     = old,
    };
    vkCheck(vkCreateSwapchainKHR(dev, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    if (old) vkDestroySwapchainKHR(dev, old, nullptr);

    images_ = enumerate<VkImage>(
        [&](uint32_t* n, VkImage* img) { return vkGetSwapchainImagesKHR(dev, swapchain_, n, img); },
        "vkGetSwapchainImagesKHR");

    // One render-finished semaphore PER SWAPCHAIN IMAGE: a single shared one
    // could be re-signalled by the next submit while a previous present still
    // waits on it (a binary semaphore must be unsignalled when signalled).
    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(dev, s, nullptr);
    renderFinished_.assign(images_.size(), VK_NULL_HANDLE);
    const VkSemaphoreCreateInfo sci{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (VkSemaphore& s : renderFinished_) vkCheck(vkCreateSemaphore(dev, &sci, nullptr, &s), "vkCreateSemaphore");

    stale_ = false;
    return true;
}

std::optional<Presenter::Frame> Presenter::beginFrame() {
    if (stale_ && !recreateSwapchain()) return std::nullopt;

    const VkDevice dev = vk_.device();
    vkCheck(vkWaitForFences(dev, 1, &inFlight_, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    uint32_t imageIndex = 0;
    const VkResult acquired = vkAcquireNextImageKHR(dev, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { stale_ = true; return std::nullopt; }
    if (acquired == VK_SUBOPTIMAL_KHR) stale_ = true;   // still usable; recreate after this frame
    else vkCheck(acquired, "vkAcquireNextImageKHR");

    // Reset only once we are committed to submitting, so an early return can
    // never leave the fence unsignalled (which would freeze the next frame).
    vkCheck(vkResetFences(dev, 1, &inFlight_), "vkResetFences");
    vkCheck(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
    const VkCommandBufferBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkCheck(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
    return Frame{ .cmd = cmd_, .imageIndex = imageIndex };
}

void Presenter::endFrame(const Frame& frame, std::span<const VkImage> sources, VkExtent2D sourceExtent) {
    const VkCommandBuffer c      = frame.cmd;
    const VkImage         target = images_[frame.imageIndex];

    Barriers().image(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     { BLIT_STAGE, VK_ACCESS_2_NONE }, { BLIT_STAGE, VK_ACCESS_2_TRANSFER_WRITE_BIT })
              .record(c);

    // Each source fills its slice of the image. A blit (not a copy) converts
    // RGBA -> BGRA swapchain formats, and scales if the surface size differs
    // from the window's logical size (DPI scaling).
    const auto count  = static_cast<uint32_t>(sources.size());
    const auto sliceW = static_cast<int32_t>(extent_.width / std::max(count, 1u));
    const auto height = static_cast<int32_t>(extent_.height);
    const bool exact  = sliceW == static_cast<int32_t>(sourceExtent.width) && height == static_cast<int32_t>(sourceExtent.height);
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t x0 = static_cast<int32_t>(i) * sliceW;
        const VkImageBlit2 region{
            .sType          = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets     = { { 0, 0, 0 }, { static_cast<int32_t>(sourceExtent.width), static_cast<int32_t>(sourceExtent.height), 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets     = { { x0, 0, 0 }, { x0 + sliceW, height, 1 } },
        };
        const VkBlitImageInfo2 blit{
            .sType          = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
            .srcImage       = sources[i],
            .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .dstImage       = target,
            .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .regionCount    = 1,
            .pRegions       = &region,
            .filter         = exact ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
        };
        vkCmdBlitImage2(c, &blit);
    }

    // To the presentation engine. The barrier ends in the stage the
    // render-finished semaphore is signalled at, which chains this layout
    // transition into what the present waits for.
    Barriers().image(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     { BLIT_STAGE, VK_ACCESS_2_TRANSFER_WRITE_BIT }, { PRESENT_STAGE, VK_ACCESS_2_NONE })
              .record(c);
    vkCheck(vkEndCommandBuffer(c), "vkEndCommandBuffer");

    // Only the blit waits for the acquire: the ray tracing earlier in the
    // command buffer does not touch the swapchain image, so it overlaps the
    // presentation engine's latency.
    const VkSemaphoreSubmitInfo wait{
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = imageAvailable_,
        .stageMask = BLIT_STAGE,
    };
    const VkSemaphoreSubmitInfo signal{
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = renderFinished_[frame.imageIndex],
        .stageMask = PRESENT_STAGE,
    };
    const VkCommandBufferSubmitInfo cmdInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = c };
    const VkSubmitInfo2 submit{
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount   = 1,
        .pWaitSemaphoreInfos      = &wait,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cmdInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signal,
    };
    vkCheck(vkQueueSubmit2(vk_.queue(), 1, &submit, inFlight_), "vkQueueSubmit2");

    const VkPresentInfoKHR present{
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &renderFinished_[frame.imageIndex],
        .swapchainCount     = 1,
        .pSwapchains        = &swapchain_,
        .pImageIndices      = &frame.imageIndex,
    };
    const VkResult presented = vkQueuePresentKHR(vk_.queue(), &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) stale_ = true;
    else vkCheck(presented, "vkQueuePresentKHR");
}
