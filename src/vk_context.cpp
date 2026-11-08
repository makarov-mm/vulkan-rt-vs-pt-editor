// Vulkan instance, device and queue selection, ray tracing entry points and
// the buffer / image / shader helpers.
#include "vk_context.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"

namespace {
constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

constexpr VkMemoryPropertyFlags HOST_MEMORY = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

std::vector<const char*> deviceExtensions(bool present) {
    std::vector<const char*> e{
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,   // required by VK_KHR_acceleration_structure
    };
    if (present) e.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    return e;
}

bool layerAvailable(const char* name) {
    const auto layers = enumerate<VkLayerProperties>(
        [](uint32_t* n, VkLayerProperties* p) { return vkEnumerateInstanceLayerProperties(n, p); },
        "vkEnumerateInstanceLayerProperties");
    return std::ranges::any_of(layers, [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::println(stderr, "[Vulkan] {}", data->pMessage);
    return VK_FALSE;
}

// The feature chain this project needs, queried or enabled in one go.
struct DeviceFeatures {
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR    rt{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &rt };
    VkPhysicalDeviceVulkan13Features                 v13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &as };
    VkPhysicalDeviceVulkan12Features                 v12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13 };

    DeviceFeatures() = default;
    DeviceFeatures(const DeviceFeatures&) = CXX26_DELETED("the pNext chain points into this object");

    [[nodiscard]] bool supported() const noexcept {
        return rt.rayTracingPipeline && as.accelerationStructure && v12.bufferDeviceAddress && v13.synchronization2;
    }
    void enableRequired() noexcept {
        rt.rayTracingPipeline = as.accelerationStructure = v12.bufferDeviceAddress = v13.synchronization2 = VK_TRUE;
    }
};

std::string toUtf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return { reinterpret_cast<const char*>(s.data()), s.size() };
}

template <class Fn>
void loadDeviceFunction(VkDevice dev, Fn& fn, const char* name) {
    fn = reinterpret_cast<Fn>(vkGetDeviceProcAddr(dev, name));
    if (!fn) throw std::runtime_error(std::format("Failed to load {}", name));
}
} // namespace

VulkanContext::VulkanContext(std::span<const char* const> surfaceExtensions, const SurfaceFactory& createSurface) {
    try {
        createInstance(surfaceExtensions);
        if (createSurface) surface_ = createSurface(instance_);
        pickPhysicalDevice();
        createDevice();
    } catch (...) {
        destroy();
        throw;
    }
}

VulkanContext::~VulkanContext() {
    destroy();
}

void VulkanContext::destroy() noexcept {
    if (dev_) {
        vkDeviceWaitIdle(dev_);
        if (cmdPool_) vkDestroyCommandPool(dev_, cmdPool_, nullptr);   // frees its command buffers
        vkDestroyDevice(dev_, nullptr);
        cmdPool_ = VK_NULL_HANDLE;
        dev_     = VK_NULL_HANDLE;
    }
    if (instance_) {
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (debugMessenger_) {
            const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyMessenger) destroyMessenger(instance_, debugMessenger_, nullptr);
        }
        vkDestroyInstance(instance_, nullptr);
        surface_        = VK_NULL_HANDLE;
        debugMessenger_ = VK_NULL_HANDLE;
        instance_       = VK_NULL_HANDLE;
    }
}

// =============================================================================
//  Instance, physical device, device
// =============================================================================
void VulkanContext::createInstance(std::span<const char* const> surfaceExtensions) {
    std::vector<const char*> extensions(surfaceExtensions.begin(), surfaceExtensions.end());
    std::vector<const char*> layers;

    // Validation is optional: a Debug build must still start on a machine
    // without the Vulkan SDK (enabling a missing layer fails vkCreateInstance).
    bool validation = false;
    if constexpr (ENABLE_VALIDATION) {
        validation = layerAvailable(VALIDATION_LAYER);
        if (!validation) std::println(stderr, "{} not found; running without validation.", VALIDATION_LAYER);
    }
    if (validation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back(VALIDATION_LAYER);
    }

    const VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debugCallback,
    };
    const VkApplicationInfo app{
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "VkRtSceneEditor",
        .apiVersion       = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo ci{
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = validation ? &messengerInfo : nullptr,   // also covers vkCreate/DestroyInstance
        .pApplicationInfo        = &app,
        .enabledLayerCount       = static_cast<uint32_t>(layers.size()),
        .ppEnabledLayerNames     = layers.data(),
        .enabledExtensionCount   = static_cast<uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    vkCheck(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance");

    if (validation) {
        const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger)
            vkCheck(createMessenger(instance_, &messengerInfo, nullptr, &debugMessenger_), "vkCreateDebugUtilsMessengerEXT");
    }
}

void VulkanContext::pickPhysicalDevice() {
    const auto devices = enumerate<VkPhysicalDevice>(
        [&](uint32_t* n, VkPhysicalDevice* d) { return vkEnumeratePhysicalDevices(instance_, n, d); },
        "vkEnumeratePhysicalDevices");
    const auto required = deviceExtensions(surface_ != VK_NULL_HANDLE);

    int bestScore = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;

        const auto exts = enumerate<VkExtensionProperties>(
            [&](uint32_t* n, VkExtensionProperties* p) { return vkEnumerateDeviceExtensionProperties(d, nullptr, n, p); },
            "vkEnumerateDeviceExtensionProperties");
        const bool hasAll = std::ranges::all_of(required, [&](const char* name) {
            return std::ranges::any_of(exts, [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
        });
        if (!hasAll) continue;

        DeviceFeatures features;
        VkPhysicalDeviceFeatures2 f2{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features.v12 };
        vkGetPhysicalDeviceFeatures2(d, &f2);
        if (!features.supported()) continue;

        // One queue family for everything: graphics (the blit), compute (the
        // denoiser), ray tracing, and presentation when there is a surface.
        const auto families = enumerate<VkQueueFamilyProperties>(
            [&](uint32_t* n, VkQueueFamilyProperties* p) { vkGetPhysicalDeviceQueueFamilyProperties(d, n, p); },
            "vkGetPhysicalDeviceQueueFamilyProperties");
        std::optional<uint32_t> family;
        for (uint32_t i = 0; i < families.size() && !family; ++i) {
            constexpr VkQueueFlags needed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[i].queueFlags & needed) != needed) continue;
            VkBool32 present = VK_TRUE;
            if (surface_)
                vkCheck(vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present), "vkGetPhysicalDeviceSurfaceSupportKHR");
            if (present) family = i;
        }
        if (!family) continue;

        // Prefer a discrete GPU over an integrated one on hybrid laptops.
        const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 2
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        if (score > bestScore) {
            bestScore    = score;
            phys_        = d;
            queueFamily_ = *family;
        }
    }
    if (!phys_)
        throw std::runtime_error("No GPU with Vulkan 1.3 and VK_KHR_ray_tracing_pipeline support found.\n"
                                 "A ray-tracing capable GPU and recent drivers are required.");

    asProps_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    rtProps_ = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR, .pNext = &asProps_ };
    VkPhysicalDeviceProperties2 props2{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &rtProps_ };
    vkGetPhysicalDeviceProperties2(phys_, &props2);
    vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);

    std::println("GPU: {}", props2.properties.deviceName);
}

void VulkanContext::createDevice() {
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamily_,
        .queueCount       = 1,
        .pQueuePriorities = &priority,
    };
    DeviceFeatures features;
    features.enableRequired();
    const auto extensions = deviceExtensions(surface_ != VK_NULL_HANDLE);

    const VkDeviceCreateInfo ci{
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &features.v12,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &queueInfo,
        .enabledExtensionCount   = static_cast<uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    vkCheck(vkCreateDevice(phys_, &ci, nullptr, &dev_), "vkCreateDevice");
    vkGetDeviceQueue(dev_, queueFamily_, 0, &queue_);

    loadDeviceFunction(dev_, rt_.createAccelerationStructure,           "vkCreateAccelerationStructureKHR");
    loadDeviceFunction(dev_, rt_.destroyAccelerationStructure,          "vkDestroyAccelerationStructureKHR");
    loadDeviceFunction(dev_, rt_.getAccelerationStructureBuildSizes,    "vkGetAccelerationStructureBuildSizesKHR");
    loadDeviceFunction(dev_, rt_.cmdBuildAccelerationStructures,        "vkCmdBuildAccelerationStructuresKHR");
    loadDeviceFunction(dev_, rt_.getAccelerationStructureDeviceAddress, "vkGetAccelerationStructureDeviceAddressKHR");
    loadDeviceFunction(dev_, rt_.createRayTracingPipelines,             "vkCreateRayTracingPipelinesKHR");
    loadDeviceFunction(dev_, rt_.getRayTracingShaderGroupHandles,       "vkGetRayTracingShaderGroupHandlesKHR");
    loadDeviceFunction(dev_, rt_.cmdTraceRays,                          "vkCmdTraceRaysKHR");

    const VkCommandPoolCreateInfo pci{
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily_,
    };
    vkCheck(vkCreateCommandPool(dev_, &pci, nullptr, &cmdPool_), "vkCreateCommandPool");
}

// =============================================================================
//  Buffers and images
// =============================================================================
uint32_t VulkanContext::findMemoryType(uint32_t typeBits, std::initializer_list<VkMemoryPropertyFlags> preferences) const {
    for (VkMemoryPropertyFlags wanted : preferences)
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & wanted) == wanted)
                return i;
    throw std::runtime_error("No suitable memory type");
}

Buffer VulkanContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                   std::initializer_list<VkMemoryPropertyFlags> memPreferences) {
    Buffer b{ .size = size };
    try {
        const VkBufferCreateInfo ci{
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = size,
            .usage       = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        vkCheck(vkCreateBuffer(dev_, &ci, nullptr, &b.buf), "vkCreateBuffer");

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev_, b.buf, &req);
        const uint32_t type = findMemoryType(req.memoryTypeBits, memPreferences);

        const VkMemoryAllocateFlagsInfo flags{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        const VkMemoryAllocateInfo ai{
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext           = &flags,
            .allocationSize  = req.size,
            .memoryTypeIndex = type,
        };
        vkCheck(vkAllocateMemory(dev_, &ai, nullptr, &b.mem), "vkAllocateMemory");
        vkCheck(vkBindBufferMemory(dev_, b.buf, b.mem, 0), "vkBindBufferMemory");

        if (memProps_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            vkCheck(vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    } catch (...) {
        destroy(b);
        throw;
    }
    return b;
}

Buffer VulkanContext::createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    return createBuffer(size, usage, { HOST_MEMORY | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, HOST_MEMORY });
}

Buffer VulkanContext::createDeviceBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage) {
    // Plain host memory for the staging copy: the BAR window is for per-frame data.
    Buffer staging = createBuffer(data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, { HOST_MEMORY });
    Buffer b;
    try {
        std::memcpy(staging.mapped, data.data(), data.size());
        b = createBuffer(data.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
        submitAndWait([&](VkCommandBuffer c) {
            const VkBufferCopy region{ .srcOffset = 0, .dstOffset = 0, .size = data.size() };
            vkCmdCopyBuffer(c, staging.buf, b.buf, 1, &region);
        });
    } catch (...) {
        destroy(b);
        destroy(staging);
        throw;
    }
    destroy(staging);
    return b;
}

Image VulkanContext::createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage) {
    Image img;
    try {
        const VkImageCreateInfo ci{
            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType     = VK_IMAGE_TYPE_2D,
            .format        = format,
            .extent        = { extent.width, extent.height, 1 },
            .mipLevels     = 1,
            .arrayLayers   = 1,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .tiling        = VK_IMAGE_TILING_OPTIMAL,
            .usage         = usage,
            .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        vkCheck(vkCreateImage(dev_, &ci, nullptr, &img.image), "vkCreateImage");

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev_, img.image, &req);
        const VkMemoryAllocateInfo ai{
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = req.size,
            .memoryTypeIndex = findMemoryType(req.memoryTypeBits, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT }),
        };
        vkCheck(vkAllocateMemory(dev_, &ai, nullptr, &img.mem), "vkAllocateMemory (image)");
        vkCheck(vkBindImageMemory(dev_, img.image, img.mem, 0), "vkBindImageMemory");

        const VkImageViewCreateInfo vci{
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = img.image,
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCheck(vkCreateImageView(dev_, &vci, nullptr, &img.view), "vkCreateImageView");
    } catch (...) {
        destroy(img);
        throw;
    }
    return img;
}

VkDeviceAddress VulkanContext::address(const Buffer& b) const noexcept {
    const VkBufferDeviceAddressInfo ai{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.buf };
    return vkGetBufferDeviceAddress(dev_, &ai);
}

void VulkanContext::destroy(Buffer& b) noexcept {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);   // implicitly unmaps
    b = {};
}

void VulkanContext::destroy(Image& img) noexcept {
    if (img.view)  vkDestroyImageView(dev_, img.view, nullptr);
    if (img.image) vkDestroyImage(dev_, img.image, nullptr);
    if (img.mem)   vkFreeMemory(dev_, img.mem, nullptr);
    img = {};
}

VkShaderModule VulkanContext::loadShader(const std::filesystem::path& path) const {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error(std::format("Cannot open shader: {}", toUtf8(path)));

    const auto size = static_cast<std::size_t>(f.tellg());
    if (size == 0 || size % sizeof(uint32_t) != 0)
        throw std::runtime_error(std::format("Not a SPIR-V file: {}", toUtf8(path)));

    std::vector<uint32_t> code(size / sizeof(uint32_t));   // uint32_t storage: SPIR-V alignment for free
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error(std::format("Cannot read shader: {}", toUtf8(path)));

    const VkShaderModuleCreateInfo ci{
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = code.data(),
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(dev_, &ci, nullptr, &mod), "vkCreateShaderModule");
    return mod;
}

// =============================================================================
//  One-time command buffers (startup work only)
// =============================================================================
VkCommandBuffer VulkanContext::beginOneShot() {
    const VkCommandBufferAllocateInfo ai{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = cmdPool_,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer c = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(dev_, &ai, &c), "vkAllocateCommandBuffers (one-shot)");

    const VkCommandBufferBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkCheck(vkBeginCommandBuffer(c, &bi), "vkBeginCommandBuffer (one-shot)");
    return c;
}

void VulkanContext::endOneShot(VkCommandBuffer c) {
    vkCheck(vkEndCommandBuffer(c), "vkEndCommandBuffer (one-shot)");
    const VkCommandBufferSubmitInfo cmdInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = c };
    const VkSubmitInfo2 si{
        .sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos    = &cmdInfo,
    };
    vkCheck(vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2 (one-shot)");
    vkCheck(vkQueueWaitIdle(queue_), "vkQueueWaitIdle");
    vkFreeCommandBuffers(dev_, cmdPool_, 1, &c);
}
