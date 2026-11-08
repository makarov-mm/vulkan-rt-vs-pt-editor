#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "cxx26.h"
#include "vk_context.h"

// =============================================================================
//  Swapchain and frame pacing: one frame in flight, one command buffer.
//
//      if (auto frame = presenter.beginFrame()) {
//          ... record into frame->cmd ...
//          presenter.endFrame(*frame, images);   // blit side by side, submit, present
//      }
//
//  The swapchain is (re)created lazily, so a window that starts minimized or
//  changes DPI just skips frames until there is something to present into.
// =============================================================================
class Presenter {
public:
    struct Frame {
        VkCommandBuffer cmd        = VK_NULL_HANDLE;   // recording; submitted by endFrame
        uint32_t        imageIndex = 0;
    };

    // `preferredExtent` is used when the surface leaves the size to the swapchain.
    Presenter(VulkanContext& vk, VkExtent2D preferredExtent);
    ~Presenter();

    Presenter(const Presenter&)            = CXX26_DELETED("owns the swapchain and its semaphores; a copy would destroy them twice");
    Presenter& operator=(const Presenter&) = CXX26_DELETED("owns the swapchain and its semaphores; a copy would destroy them twice");

    // Waits until the GPU is done with the previous frame, then acquires the
    // next image and begins the frame's command buffer. nullopt when there is
    // nothing to present into right now (minimized, or the swapchain is out of
    // date): skip the frame.
    [[nodiscard]] std::optional<Frame> beginFrame();

    // Blits `sources` (TRANSFER_SRC_OPTIMAL, `sourceExtent` each) side by side
    // across the frame's image, then submits and presents.
    void endFrame(const Frame& frame, std::span<const VkImage> sources, VkExtent2D sourceExtent);

private:
    bool recreateSwapchain();   // false while the surface has no area
    void destroySwapchain() noexcept;
    void destroy() noexcept;

    VulkanContext& vk_;
    VkExtent2D     preferredExtent_;

    VkSwapchainKHR       swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR   format_{};
    VkExtent2D           extent_{};
    std::vector<VkImage> images_;
    bool                 stale_ = true;   // out of date / suboptimal / not created yet

    VkCommandBuffer          cmd_            = VK_NULL_HANDLE;
    VkFence                  inFlight_       = VK_NULL_HANDLE;
    VkSemaphore              imageAvailable_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> renderFinished_;   // one per swapchain image, see recreateSwapchain
};
