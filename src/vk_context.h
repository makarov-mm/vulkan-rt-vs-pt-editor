#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <span>
#include <utility>

#include <vulkan/vulkan.h>

#include "cxx26.h"
#include "vk_utils.h"

// Device-level entry points of VK_KHR_acceleration_structure and
// VK_KHR_ray_tracing_pipeline. Extension functions are not exported by the
// loader library, so they are fetched from the device at startup.
struct RayTracingFunctions {
    PFN_vkCreateAccelerationStructureKHR           createAccelerationStructure           = nullptr;
    PFN_vkDestroyAccelerationStructureKHR          destroyAccelerationStructure          = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR    getAccelerationStructureBuildSizes    = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR        cmdBuildAccelerationStructures        = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelerationStructureDeviceAddress = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR             createRayTracingPipelines             = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR       getRayTracingShaderGroupHandles       = nullptr;
    PFN_vkCmdTraceRaysKHR                          cmdTraceRays                          = nullptr;
};

// =============================================================================
//  Instance, device and queue of a Vulkan 1.3 GPU with hardware ray tracing,
//  plus the resource helpers the renderer and the presenter share.
//
//  Platform independent: window-system integration comes in through the
//  surface extensions and the surface factory (none for headless use).
// =============================================================================
class VulkanContext {
public:
    using SurfaceFactory = std::function<VkSurfaceKHR(VkInstance)>;

    // With a surface factory the chosen queue must also be able to present to
    // the surface it creates, and VK_KHR_swapchain is enabled.
    VulkanContext(std::span<const char* const> surfaceExtensions, const SurfaceFactory& createSurface);
    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = CXX26_DELETED("owns the Vulkan instance and device; a copy would destroy them twice");
    VulkanContext& operator=(const VulkanContext&) = CXX26_DELETED("owns the Vulkan instance and device; a copy would destroy them twice");

    [[nodiscard]] VkInstance       instance()       const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return phys_; }
    [[nodiscard]] VkDevice         device()         const noexcept { return dev_; }
    [[nodiscard]] VkQueue          queue()          const noexcept { return queue_; }
    [[nodiscard]] VkSurfaceKHR     surface()        const noexcept { return surface_; }
    [[nodiscard]] VkCommandPool    commandPool()    const noexcept { return cmdPool_; }

    [[nodiscard]] const RayTracingFunctions& rt() const noexcept { return rt_; }
    [[nodiscard]] const VkPhysicalDeviceRayTracingPipelinePropertiesKHR&    rtProperties() const noexcept { return rtProps_; }
    [[nodiscard]] const VkPhysicalDeviceAccelerationStructurePropertiesKHR& asProperties() const noexcept { return asProps_; }

    // ---- Resources ----------------------------------------------------------
    // Every buffer gets a device address. Host-visible ones stay mapped.
    // The memory type is the first one allowed for the buffer that has all
    // flags of the earliest satisfiable entry in `memPreferences`.
    [[nodiscard]] Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                      std::initializer_list<VkMemoryPropertyFlags> memPreferences);
    // Small buffers the CPU rewrites every frame: device-local host-visible
    // memory (resizable BAR) when available, plain host memory otherwise.
    [[nodiscard]] Buffer createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    // Static data read by the GPU: uploaded once through a staging buffer.
    [[nodiscard]] Buffer createDeviceBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage);
    [[nodiscard]] Image  createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage);

    [[nodiscard]] VkDeviceAddress address(const Buffer& b) const noexcept;
    [[nodiscard]] VkShaderModule  loadShader(const std::filesystem::path& path) const;

    void destroy(Buffer& b) noexcept;
    void destroy(Image& img) noexcept;

    // Records `record` into a one-time command buffer, submits it and waits.
    template <std::invocable<VkCommandBuffer> Record>
    void submitAndWait(Record&& record) {
        const VkCommandBuffer c = beginOneShot();
        std::forward<Record>(record)(c);
        endOneShot(c);
    }

private:
    void createInstance(std::span<const char* const> surfaceExtensions);
    void pickPhysicalDevice();
    void createDevice();
    void destroy() noexcept;   // idempotent; also runs when the constructor throws halfway

    [[nodiscard]] uint32_t findMemoryType(uint32_t typeBits, std::initializer_list<VkMemoryPropertyFlags> preferences) const;
    [[nodiscard]] VkCommandBuffer beginOneShot();
    void endOneShot(VkCommandBuffer c);   // submits, waits, frees

    VkInstance               instance_       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice         phys_           = VK_NULL_HANDLE;
    VkDevice                 dev_            = VK_NULL_HANDLE;
    uint32_t                 queueFamily_    = 0;
    VkQueue                  queue_          = VK_NULL_HANDLE;
    VkCommandPool            cmdPool_        = VK_NULL_HANDLE;

    VkPhysicalDeviceMemoryProperties                   memProps_{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR    rtProps_{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps_{};
    RayTracingFunctions                                rt_;
};
