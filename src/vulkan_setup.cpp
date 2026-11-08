// Vulkan setup: instance, device, swapchain, resources, acceleration
// structures, pipelines, shader binding tables and the denoiser.
#include "scene_editor.h"

#include <cstring>
#include <format>
#include <fstream>
#include <numbers>
#include <print>
#include <stdexcept>
#include <string>

#include "rt_functions.h"

namespace {
constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

constexpr std::array DEVICE_EXTENSIONS{
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};

template <class T, class Fn>
std::vector<T> enumerate(Fn fn) {
    uint32_t n = 0;
    fn(&n, nullptr);
    std::vector<T> v(n);
    fn(&n, v.data());
    v.resize(n);
    return v;
}

bool layerAvailable(const char* name) {
    const auto layers = enumerate<VkLayerProperties>(
        [](uint32_t* n, VkLayerProperties* p) { vkEnumerateInstanceLayerProperties(n, p); });
    return std::ranges::any_of(layers, [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::println(stderr, "[Vulkan] {}", data->pMessage);
    return VK_FALSE;
}

// Destroys shader modules when pipeline creation is done (or throws).
struct ShaderModules {
    VkDevice dev;
    std::vector<VkShaderModule> modules;
    ~ShaderModules() { for (VkShaderModule m : modules) vkDestroyShaderModule(dev, m, nullptr); }
    VkShaderModule add(VkShaderModule m) { modules.push_back(m); return m; }
};
} // namespace

void imageBarrier(VkCommandBuffer c, VkImage img,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask       = srcAccess;
    b.dstAccessMask       = dstAccess;
    b.oldLayout           = oldLayout;
    b.newLayout           = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void SceneEditor::initVulkan() {
    createInstance();
    createSurface();
    pickPhysicalDevice();
    createDevice();
    loadRayTracingFunctions(dev);
    createSwapchain();
    createCommandResources();
    createPrototypeMeshes();
    createDefaultScene();
    createAccelStructures();
    createImages();
    createUniformBuffer();
    createDescriptors();
    createRayTracingPipelines();
    createShaderBindingTables();
    createDenoiser();
    startTime = std::chrono::steady_clock::now();
}

// =============================================================================
//  Instance, surface, device
// =============================================================================
void SceneEditor::createInstance() {
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "VkRtSceneEditor";
    app.apiVersion       = VK_API_VERSION_1_2;

    std::vector<const char*> extensions{ VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    std::vector<const char*> layers;

    // Validation is optional: a Debug build must still start on a machine
    // without the Vulkan SDK (enabling a missing layer fails vkCreateInstance).
    const bool validation = ENABLE_VALIDATION && layerAvailable(VALIDATION_LAYER);
    if (validation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back(VALIDATION_LAYER);
    } else if (ENABLE_VALIDATION) {
        std::println(stderr, "{} not found; running without validation.", VALIDATION_LAYER);
    }

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames     = layers.data();
    vkCheck(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");

    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT d{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            d.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            d.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            d.pfnUserCallback = debugCallback;
            vkCheck(create(instance, &d, nullptr, &debugMessenger), "vkCreateDebugUtilsMessengerEXT");
        }
    }
}

void SceneEditor::createSurface() {
    VkWin32SurfaceCreateInfoKHR ci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd      = viewHwnd;   // the swapchain lives in the viewport child window
    vkCheck(vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface), "vkCreateWin32SurfaceKHR");
}

void SceneEditor::pickPhysicalDevice() {
    const auto devices = enumerate<VkPhysicalDevice>(
        [&](uint32_t* n, VkPhysicalDevice* d) { vkEnumeratePhysicalDevices(instance, n, d); });
    if (devices.empty()) throw std::runtime_error("No Vulkan devices found");

    int bestScore = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_2) continue;

        const auto exts = enumerate<VkExtensionProperties>(
            [&](uint32_t* n, VkExtensionProperties* p) { vkEnumerateDeviceExtensionProperties(d, nullptr, n, p); });
        const bool hasAll = std::ranges::all_of(DEVICE_EXTENSIONS, [&](const char* name) {
            return std::ranges::any_of(exts, [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
        });
        if (!hasAll) continue;

        // Required features.
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rtFeat };
        VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &asFeat };
        VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
        vkGetPhysicalDeviceFeatures2(d, &f2);
        if (!rtFeat.rayTracingPipeline || !asFeat.accelerationStructure || !v12.bufferDeviceAddress) continue;

        // A queue family that does graphics (for the blit), compute and present.
        const auto families = enumerate<VkQueueFamilyProperties>(
            [&](uint32_t* n, VkQueueFamilyProperties* p) { vkGetPhysicalDeviceQueueFamilyProperties(d, n, p); });
        std::optional<uint32_t> family;
        for (uint32_t i = 0; i < families.size() && !family; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
            constexpr VkQueueFlags needed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[i].queueFlags & needed) == needed && present) family = i;
        }
        if (!family) continue;

        // Prefer a discrete GPU over an integrated one on hybrid laptops.
        const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        if (score > bestScore) {
            bestScore   = score;
            phys        = d;
            queueFamily = *family;
        }
    }
    if (!phys)
        throw std::runtime_error("No GPU with VK_KHR_ray_tracing_pipeline support found.\n"
                                 "A ray-tracing capable GPU and recent drivers are required.");

    asProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    rtProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR, &asProps };
    VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &rtProps };
    vkGetPhysicalDeviceProperties2(phys, &props2);
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    std::println("GPU: {}", props2.properties.deviceName);
}

void SceneEditor::createDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    q.queueFamilyIndex = queueFamily;
    q.queueCount       = 1;
    q.pQueuePriorities = &priority;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    rtFeat.rayTracingPipeline = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rtFeat };
    asFeat.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &asFeat };
    v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };

    VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2 };
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &q;
    ci.enabledExtensionCount   = static_cast<uint32_t>(DEVICE_EXTENSIONS.size());
    ci.ppEnabledExtensionNames = DEVICE_EXTENSIONS.data();
    vkCheck(vkCreateDevice(phys, &ci, nullptr, &dev), "vkCreateDevice");

    vkGetDeviceQueue(dev, queueFamily, 0, &queue);
}

// =============================================================================
//  Swapchain
// =============================================================================
void SceneEditor::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        throw std::runtime_error("The surface does not support blitting into swapchain images");

    // The shaders write display-referred (already gamma-encoded) values, so
    // prefer a UNORM format; an sRGB format would apply gamma twice.
    const auto formats = enumerate<VkSurfaceFormatKHR>(
        [&](uint32_t* n, VkSurfaceFormatKHR* f) { vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, n, f); });
    if (formats.empty()) throw std::runtime_error("The surface reports no formats");
    swapFormat = formats.front();
    for (const VkSurfaceFormatKHR& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { swapFormat = f; break; }
    }

    swapExtent = caps.currentExtent;
    if (swapExtent.width == UINT32_MAX) swapExtent = { WIDTH * VIEWS, HEIGHT };

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    const VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface          = surface;
    ci.minImageCount    = imageCount;
    ci.imageFormat      = swapFormat.format;
    ci.imageColorSpace  = swapFormat.colorSpace;
    ci.imageExtent      = swapExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = old;
    vkCheck(vkCreateSwapchainKHR(dev, &ci, nullptr, &swapchain), "vkCreateSwapchainKHR");
    if (old) vkDestroySwapchainKHR(dev, old, nullptr);

    swapImages = enumerate<VkImage>(
        [&](uint32_t* n, VkImage* img) { vkGetSwapchainImagesKHR(dev, swapchain, n, img); });

    // One render-finished semaphore PER SWAPCHAIN IMAGE: a single shared one
    // could be re-signalled by the next submit while a previous present still
    // waits on it (VUID-vkQueueSubmit-pSignalSemaphores-00067).
    for (VkSemaphore s : semRenderFinished) vkDestroySemaphore(dev, s, nullptr);
    semRenderFinished.assign(swapImages.size(), VK_NULL_HANDLE);
    const VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (VkSemaphore& s : semRenderFinished) vkCheck(vkCreateSemaphore(dev, &sci, nullptr, &s));
}

bool SceneEditor::recreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;   // minimized

    vkCheck(vkDeviceWaitIdle(dev));
    createSwapchain();   // passes the current swapchain as oldSwapchain and retires it
    swapchainStale = false;
    return true;
}

void SceneEditor::destroySwapchain() {
    for (VkSemaphore s : semRenderFinished) vkDestroySemaphore(dev, s, nullptr);
    semRenderFinished.clear();
    if (swapchain) vkDestroySwapchainKHR(dev, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
    swapImages.clear();
}

void SceneEditor::createCommandResources() {
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    vkCheck(vkCreateCommandPool(dev, &pci, nullptr, &cmdPool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = cmdPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(dev, &ai, &cmd), "vkAllocateCommandBuffers");

    const VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCheck(vkCreateSemaphore(dev, &sci, nullptr, &semImageAvailable));

    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCheck(vkCreateFence(dev, &fci, nullptr, &inFlight));
}

// =============================================================================
//  Memory helpers
// =============================================================================
uint32_t SceneEditor::findMemoryType(uint32_t typeBits, std::initializer_list<VkMemoryPropertyFlags> preferences) const {
    for (VkMemoryPropertyFlags wanted : preferences)
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & wanted) == wanted)
                return i;
    throw std::runtime_error("No suitable memory type");
}

Buffer SceneEditor::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 std::initializer_list<VkMemoryPropertyFlags> memPreferences) {
    Buffer b{ .size = size };

    VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size        = size;
    ci.usage       = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(dev, &ci, nullptr, &b.buf), "vkCreateBuffer");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buf, &req);

    VkMemoryAllocateFlagsInfo flags{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    const uint32_t type = findMemoryType(req.memoryTypeBits, memPreferences);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = type;
    vkCheck(vkAllocateMemory(dev, &ai, nullptr, &b.mem), "vkAllocateMemory");
    vkCheck(vkBindBufferMemory(dev, b.buf, b.mem, 0), "vkBindBufferMemory");

    // Host-visible buffers stay mapped for their whole lifetime.
    if (memProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkCheck(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    return b;
}

// Small buffers the CPU rewrites and the GPU reads every frame. Device-local
// host-visible memory (resizable BAR) keeps the shader reads in VRAM; plain
// host memory is the fallback.
Buffer SceneEditor::createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    constexpr VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    return createBuffer(size, usage, { host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, host });
}

// Static data read by the GPU on every ray: uploaded once through a staging
// buffer into device-local memory.
Buffer SceneEditor::createDeviceBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage) {
    Buffer staging = createHostBuffer(data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, data.data(), data.size());

    Buffer b = createBuffer(data.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
    VkCommandBuffer c = beginOneShot();
    const VkBufferCopy region{ 0, 0, data.size() };
    vkCmdCopyBuffer(c, staging.buf, b.buf, 1, &region);
    submitOneShot(c);

    destroyBuffer(staging);
    return b;
}

Image SceneEditor::createImage(VkFormat format, VkImageUsageFlags usage) {
    Image img;

    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = format;
    ci.extent        = { WIDTH, HEIGHT, 1 };   // per-view size, not the window size
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(dev, &ci, nullptr, &img.image), "vkCreateImage");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img.image, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
    vkCheck(vkAllocateMemory(dev, &ai, nullptr, &img.mem), "vkAllocateMemory (image)");
    vkCheck(vkBindImageMemory(dev, img.image, img.mem, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image            = img.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = format;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCheck(vkCreateImageView(dev, &vci, nullptr, &img.view), "vkCreateImageView");
    return img;
}

VkDeviceAddress SceneEditor::bufferAddress(const Buffer& b) const {
    VkBufferDeviceAddressInfo ai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    ai.buffer = b.buf;
    return vkGetBufferDeviceAddress(dev, &ai);
}

void SceneEditor::destroyBuffer(Buffer& b) noexcept {
    if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev, b.mem, nullptr);   // implicitly unmaps
    b = {};
}

void SceneEditor::destroyImage(Image& img) noexcept {
    if (img.view)  vkDestroyImageView(dev, img.view, nullptr);
    if (img.image) vkDestroyImage(dev, img.image, nullptr);
    if (img.mem)   vkFreeMemory(dev, img.mem, nullptr);
    img = {};
}

VkCommandBuffer SceneEditor::beginOneShot() {
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = cmdPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer c = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(dev, &ai, &c), "vkAllocateCommandBuffers (one-shot)");

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(c, &bi));
    return c;
}

void SceneEditor::submitOneShot(VkCommandBuffer c) {
    vkCheck(vkEndCommandBuffer(c));
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &c;
    vkCheck(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (one-shot)");
    vkCheck(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, cmdPool, 1, &c);
}

// =============================================================================
//  Prototype meshes and the default scene
// =============================================================================
void SceneEditor::createPrototypeMeshes() {
    MeshBuilder b;
    auto build = [&](MeshType type, auto&& generate) {
        b.begin();
        generate();
        static_cast<MeshRange&>(mesh(type)) = b.end();
    };
    build(MeshType::Sphere,       [&] { b.sphere(SPHERE_RADIUS); });
    build(MeshType::Diamond,      [&] { b.diamond(1.25f); });
    build(MeshType::Cube,         [&] { b.cube(2.0f); });
    build(MeshType::Pyramid,      [&] { b.pyramid(2.2f, 2.4f); });
    build(MeshType::Cylinder,     [&] { b.cylinder(1.05f, 2.4f, 56); });
    build(MeshType::Dodecahedron, [&] { b.dodecahedron(1.55f); });
    build(MeshType::Supertoroid,  [&] { b.supertoroid(); });
    build(MeshType::Supershape,   [&] { b.supershape(); });
    build(MeshType::Floor,        [&] { b.floor(20.0f); });

    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    vertexBuffer = createDeviceBuffer(std::as_bytes(std::span(b.vertices())), usage);
    indexBuffer  = createDeviceBuffer(std::as_bytes(std::span(b.indices())),  usage);

    std::println("Prototype meshes: {} vertices, {} triangles across {} BLASes",
                 b.vertices().size(), b.indices().size() / 3, MESH_COUNT);
}

// The classic scene: a central twisted supertoroid and six coloured glass
// shapes on a ring, each resting on the floor.
void SceneEditor::createDefaultScene() {
    auto add = [&](MeshType type, Vec3 pos, Vec3 color, Mat3 rot = {}) {
        objects.push_back({ .mesh = type, .pos = pos, .rot = rot, .color = color, .material = Material::Glass });
    };
    auto onRing = [&](int k, MeshType type) {
        constexpr float ring = 6.2f;
        const float ang = static_cast<float>(k) / 6.0f * 2.0f * std::numbers::pi_v<float>;
        return Vec3{ std::cos(ang) * ring, -mesh(type).minY, std::sin(ang) * ring };
    };

    add(MeshType::Supertoroid, { 0.0f, 2.7f, 0.0f }, { 1.0f, 1.0f, 1.0f },
        Mat3::rotation({ 1.0f, 0.25f, 0.35f }, 0.7f));

    constexpr std::array<std::pair<MeshType, Vec3>, 6> ring{ {
        { MeshType::Sphere,       { 0.55f, 0.72f, 1.00f } },
        { MeshType::Diamond,      { 1.00f, 0.65f, 0.72f } },
        { MeshType::Cube,         { 0.55f, 0.95f, 0.65f } },
        { MeshType::Pyramid,      { 1.00f, 0.85f, 0.45f } },
        { MeshType::Cylinder,     { 0.45f, 0.92f, 0.92f } },
        { MeshType::Dodecahedron, { 0.78f, 0.60f, 1.00f } },
    } };
    for (int k = 0; const auto& [type, color] : ring) add(type, onRing(k++, type), color);

    sceneDirty = true;
}

// =============================================================================
//  Acceleration structures: one BLAS per mesh (built once), one TLAS with an
//  instance per scene object (rebuilt in the frame command buffer on edits).
// =============================================================================
void SceneEditor::createAccelStructures() {
    const VkDeviceAddress vtxAddr = bufferAddress(vertexBuffer);
    const VkDeviceAddress idxAddr = bufferAddress(indexBuffer);
    const VkDeviceSize scratchAlign = asProps.minAccelerationStructureScratchOffsetAlignment;

    // ---- size and create every BLAS ----
    std::array<VkAccelerationStructureGeometryKHR, MESH_COUNT>          geoms{};
    std::array<VkAccelerationStructureBuildGeometryInfoKHR, MESH_COUNT> infos{};
    std::array<VkAccelerationStructureBuildRangeInfoKHR, MESH_COUNT>    ranges{};
    std::array<VkDeviceSize, MESH_COUNT>                                scratchOffsets{};
    VkDeviceSize blasScratchTotal = 0;

    for (std::size_t m = 0; m < MESH_COUNT; ++m) {
        MeshInfo& mi = meshes[m];

        VkAccelerationStructureGeometryKHR& g = geoms[m];
        g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tri = g.geometry.triangles;
        tri.sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = vtxAddr;
        tri.vertexStride             = sizeof(Vertex);
        tri.maxVertex                = mi.firstVert + mi.vertCount - 1;
        tri.indexType                = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress  = idxAddr + mi.firstIndex * sizeof(uint32_t);

        VkAccelerationStructureBuildGeometryInfoKHR& bi = infos[m];
        bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bi.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;   // static, built once
        bi.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries   = &g;

        ranges[m].primitiveCount = mi.indexCount / 3;
        VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        pvkGetAccelerationStructureBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                              &bi, &ranges[m].primitiveCount, &sizes);

        mi.blasBuf = createBuffer(sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
        VkAccelerationStructureCreateInfoKHR ci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        ci.buffer = mi.blasBuf.buf;
        ci.size   = sizes.accelerationStructureSize;
        ci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        vkCheck(pvkCreateAccelerationStructure(dev, &ci, nullptr, &mi.blas), "create BLAS");

        // Each BLAS gets its own scratch slice, so all of them build in one call.
        scratchOffsets[m] = blasScratchTotal;
        blasScratchTotal += alignUp(sizes.buildScratchSize, scratchAlign);
    }

    // ---- TLAS sized for the maximum instance count ----
    instanceBuffer = createHostBuffer(MAX_INSTANCES * sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    objDataBuffer = createHostBuffer(MAX_INSTANCES * sizeof(GpuObj), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    tlasGeom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeom.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tlasGeom.geometry.instances.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlasGeom.geometry.instances.arrayOfPointers    = VK_FALSE;
    tlasGeom.geometry.instances.data.deviceAddress = bufferAddress(instanceBuffer);

    VkAccelerationStructureBuildGeometryInfoKHR ti{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    ti.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    ti.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    ti.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    ti.geometryCount = 1;
    ti.pGeometries   = &tlasGeom;

    const uint32_t maxInstances = MAX_INSTANCES;
    VkAccelerationStructureBuildSizesInfoKHR ts{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    pvkGetAccelerationStructureBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &maxInstances, &ts);

    tlasBuffer = createBuffer(ts.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
    VkAccelerationStructureCreateInfoKHR tci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    tci.buffer = tlasBuffer.buf;
    tci.size   = ts.accelerationStructureSize;
    tci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    vkCheck(pvkCreateAccelerationStructure(dev, &tci, nullptr, &tlas), "create TLAS");

    // ---- one scratch buffer: BLAS slices at startup, reused by every TLAS rebuild ----
    // Over-allocated by one alignment unit so the start can be aligned
    // (scratch addresses must be multiples of minAccelerationStructureScratchOffsetAlignment).
    asScratch = createBuffer(std::max(blasScratchTotal, ts.buildScratchSize) + scratchAlign,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
    scratchAddr = alignUp(bufferAddress(asScratch), scratchAlign);

    // ---- build every BLAS in a single command ----
    std::array<const VkAccelerationStructureBuildRangeInfoKHR*, MESH_COUNT> rangePtrs{};
    for (std::size_t m = 0; m < MESH_COUNT; ++m) {
        infos[m].dstAccelerationStructure  = meshes[m].blas;
        infos[m].scratchData.deviceAddress = scratchAddr + scratchOffsets[m];
        rangePtrs[m] = &ranges[m];
    }
    VkCommandBuffer c = beginOneShot();
    pvkCmdBuildAccelerationStructures(c, static_cast<uint32_t>(MESH_COUNT), infos.data(), rangePtrs.data());
    submitOneShot(c);

    for (MeshInfo& mi : meshes) {
        VkAccelerationStructureDeviceAddressInfoKHR ai{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        ai.accelerationStructure = mi.blas;
        mi.blasAddr = pvkGetAccelerationStructureDeviceAddress(dev, &ai);
    }

    sceneDirty = true;   // the first frame writes the instances and builds the TLAS
}

// =============================================================================
//  Images, uniform buffer, descriptors
// =============================================================================
void SceneEditor::createImages() {
    constexpr VkFormat HDR = VK_FORMAT_R32G32B32A32_SFLOAT;
    constexpr VkFormat LDR = VK_FORMAT_R8G8B8A8_UNORM;

    for (RendererView& v : views) {
        v.output  = createImage(LDR, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        v.accum   = createImage(HDR, VK_IMAGE_USAGE_STORAGE_BIT);
        v.history = createImage(HDR, VK_IMAGE_USAGE_STORAGE_BIT);
    }
    guide  = createImage(HDR, VK_IMAGE_USAGE_STORAGE_BIT);
    albedo = createImage(LDR, VK_IMAGE_USAGE_STORAGE_BIT);
    ping   = createImage(HDR, VK_IMAGE_USAGE_STORAGE_BIT);
    pong   = createImage(HDR, VK_IMAGE_USAGE_STORAGE_BIT);

    // One-time transition to GENERAL; these images stay in GENERAL for good.
    // (The output images are transitioned per frame around the blit.)
    VkCommandBuffer c = beginOneShot();
    auto toGeneral = [&](const Image& img) {
        imageBarrier(c, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    };
    for (const RendererView& v : views) { toGeneral(v.accum); toGeneral(v.history); }
    for (const Image* img : { &guide, &albedo, &ping, &pong }) toGeneral(*img);
    submitOneShot(c);
}

void SceneEditor::createUniformBuffer() {
    ubo = createHostBuffer(sizeof(UBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
}

void SceneEditor::createDescriptors() {
    // One layout serves both views. The Whitted ray-gen shader simply does not
    // declare bindings 8/9 (guide/albedo), which is legal.
    constexpr VkShaderStageFlags RGEN = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    constexpr VkShaderStageFlags CHIT = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    const std::array<VkDescriptorSetLayoutBinding, 10> bindings{ {
        { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, RGEN, nullptr },   // TLAS
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // output
        { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             1, RGEN, nullptr },   // camera UBO
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, CHIT, nullptr },   // vertices
        { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, CHIT, nullptr },   // indices
        { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // accumulation
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // history
        { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, CHIT, nullptr },   // object data
        { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // denoiser guide
        { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // denoiser albedo
    } };

    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = static_cast<uint32_t>(bindings.size());
    lci.pBindings    = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &descLayout), "descriptor set layout");

    // Per view: 5 storage images, 3 storage buffers, 1 UBO, 1 TLAS.
    // Denoiser: 3 sets x 5 storage images (allocated from the same pool).
    constexpr uint32_t denoiseSetCount = 3;
    const std::array<VkDescriptorPoolSize, 4> poolSizes{ {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VIEWS },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              VIEWS * 5 + denoiseSetCount * 5 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             VIEWS },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             VIEWS * 3 },
    } };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets       = VIEWS + denoiseSetCount;
    pci.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    pci.pPoolSizes    = poolSizes.data();
    vkCheck(vkCreateDescriptorPool(dev, &pci, nullptr, &descPool), "descriptor pool");

    std::array<VkDescriptorSetLayout, VIEWS> layouts;
    layouts.fill(descLayout);
    std::array<VkDescriptorSet, VIEWS> sets{};
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = descPool;
    ai.descriptorSetCount = VIEWS;
    ai.pSetLayouts        = layouts.data();
    vkCheck(vkAllocateDescriptorSets(dev, &ai, sets.data()), "allocate descriptor sets");
    for (std::size_t i = 0; i < VIEWS; ++i) views[i].descSet = sets[i];

    // The TLAS is rebuilt in place on edits, so this handle stays valid for the whole run.
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures    = &tlas;

    const VkDescriptorBufferInfo uboInfo{ ubo.buf, 0, sizeof(UBO) };
    const VkDescriptorBufferInfo vtxInfo{ vertexBuffer.buf, 0, VK_WHOLE_SIZE };
    const VkDescriptorBufferInfo idxInfo{ indexBuffer.buf, 0, VK_WHOLE_SIZE };
    const VkDescriptorBufferInfo objInfo{ objDataBuffer.buf, 0, VK_WHOLE_SIZE };
    const VkDescriptorImageInfo  guideInfo{ VK_NULL_HANDLE, guide.view,  VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorImageInfo  albInfo  { VK_NULL_HANDLE, albedo.view, VK_IMAGE_LAYOUT_GENERAL };

    // FixedVector (std::inplace_vector, C++26 P0843) guarantees by type that
    // elements never relocate, so &imgInfos.back() stays valid until
    // vkUpdateDescriptorSets consumes it.
    FixedVector<VkDescriptorImageInfo, VIEWS * 3>  imgInfos;
    FixedVector<VkWriteDescriptorSet,  VIEWS * 10> writes;

    for (const RendererView& v : views) {
        auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* img,
                         const VkDescriptorBufferInfo* buf, const void* next = nullptr) {
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, next };
            w.dstSet          = v.descSet;
            w.dstBinding      = binding;
            w.descriptorCount = 1;
            w.descriptorType  = type;
            w.pImageInfo      = img;
            w.pBufferInfo     = buf;
            writes.push_back(w);
        };
        auto image = [&](const Image& img) {
            return &imgInfos.push_back({ VK_NULL_HANDLE, img.view, VK_IMAGE_LAYOUT_GENERAL });
        };
        write(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, &asWrite);
        write(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  image(v.output), nullptr);
        write(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);
        write(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &vtxInfo);
        write(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &idxInfo);
        write(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  image(v.accum), nullptr);
        write(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  image(v.history), nullptr);
        write(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &objInfo);
        write(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  &guideInfo, nullptr);
        write(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  &albInfo, nullptr);
    }
    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// =============================================================================
//  Ray tracing pipelines (one per view; they differ only in the ray-gen shader)
// =============================================================================
VkShaderModule SceneEditor::loadShader(const char* path) const {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error(std::format("Cannot open shader: {}", path));

    const auto size = static_cast<std::size_t>(f.tellg());
    if (size == 0 || size % sizeof(uint32_t) != 0)
        throw std::runtime_error(std::format("Not a SPIR-V file: {}", path));

    std::vector<uint32_t> code(size / sizeof(uint32_t));   // uint32_t storage: SPIR-V alignment for free
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size;
    ci.pCode    = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(dev, &ci, nullptr, &mod), path);
    return mod;
}

void SceneEditor::createRayTracingPipelines() {
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount = 1;
    lci.pSetLayouts    = &descLayout;
    vkCheck(vkCreatePipelineLayout(dev, &lci, nullptr, &pipelineLayout), "pipeline layout");

    constexpr std::array<const char*, VIEWS> raygenPaths{
        "shaders/raygen_rt.rgen.spv",   // VIEW_RT: Whitted-style ray tracing
        "shaders/raygen_pt.rgen.spv",   // VIEW_PT: Monte Carlo path tracing
    };

    ShaderModules shaders{ dev };
    const VkShaderModule miss       = shaders.add(loadShader("shaders/miss.rmiss.spv"));
    const VkShaderModule shadowMiss = shaders.add(loadShader("shaders/shadow.rmiss.spv"));
    const VkShaderModule closestHit = shaders.add(loadShader("shaders/closesthit.rchit.spv"));

    auto stage = [](VkShaderStageFlagBits st, VkShaderModule m) {
        VkPipelineShaderStageCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        ci.stage  = st;
        ci.module = m;
        ci.pName  = "main";
        return ci;
    };
    auto group = [](VkRayTracingShaderGroupTypeKHR type, uint32_t general, uint32_t hit) {
        VkRayTracingShaderGroupCreateInfoKHR g{ VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
        g.type               = type;
        g.generalShader      = general;
        g.closestHitShader   = hit;
        g.anyHitShader       = VK_SHADER_UNUSED_KHR;
        g.intersectionShader = VK_SHADER_UNUSED_KHR;
        return g;
    };

    // Groups: 0 ray-gen, 1 miss, 2 shadow miss, 3 triangle hit group (matches the SBT layout).
    const std::array<VkRayTracingShaderGroupCreateInfoKHR, 4> groups{
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, VK_SHADER_UNUSED_KHR),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1, VK_SHADER_UNUSED_KHR),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 2, VK_SHADER_UNUSED_KHR),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, 3),
    };

    for (std::size_t i = 0; i < VIEWS; ++i) {
        const std::array<VkPipelineShaderStageCreateInfo, 4> stages{
            stage(VK_SHADER_STAGE_RAYGEN_BIT_KHR,      shaders.add(loadShader(raygenPaths[i]))),
            stage(VK_SHADER_STAGE_MISS_BIT_KHR,        miss),
            stage(VK_SHADER_STAGE_MISS_BIT_KHR,        shadowMiss),
            stage(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, closestHit),
        };

        VkRayTracingPipelineCreateInfoKHR pci{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
        pci.stageCount                   = static_cast<uint32_t>(stages.size());
        pci.pStages                      = stages.data();
        pci.groupCount                   = static_cast<uint32_t>(groups.size());
        pci.pGroups                      = groups.data();
        pci.maxPipelineRayRecursionDepth = 1;   // every traceRay happens in ray-gen
        pci.layout                       = pipelineLayout;
        vkCheck(pvkCreateRayTracingPipelines(dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pci, nullptr,
                                             &views[i].pipeline), "create RT pipeline");
    }
}

// =============================================================================
//  Shader binding tables (group handles are pipeline-specific: one per view)
// =============================================================================
void SceneEditor::createShaderBindingTables() {
    const uint32_t handleSize    = rtProps.shaderGroupHandleSize;
    const uint32_t handleAligned = alignUp(handleSize, rtProps.shaderGroupHandleAlignment);
    const uint32_t baseAlign     = rtProps.shaderGroupBaseAlignment;
    constexpr uint32_t missCount = 2, hitCount = 1, groupCount = 1 + missCount + hitCount;

    for (RendererView& v : views) {
        // Each region must start baseAlign-aligned; for ray-gen, size == stride.
        v.rgenRegion.stride = alignUp(handleAligned, baseAlign);
        v.rgenRegion.size   = v.rgenRegion.stride;
        v.missRegion.stride = handleAligned;
        v.missRegion.size   = alignUp(missCount * handleAligned, baseAlign);
        v.hitRegion.stride  = handleAligned;
        v.hitRegion.size    = alignUp(hitCount * handleAligned, baseAlign);
        const VkDeviceSize tableSize = v.rgenRegion.size + v.missRegion.size + v.hitRegion.size;

        // Over-allocate by baseAlign so the table start itself can be aligned.
        v.sbt = createHostBuffer(tableSize + baseAlign,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        const VkDeviceAddress raw  = bufferAddress(v.sbt);
        const VkDeviceAddress base = alignUp<VkDeviceAddress>(raw, baseAlign);
        v.rgenRegion.deviceAddress = base;
        v.missRegion.deviceAddress = base + v.rgenRegion.size;
        v.hitRegion.deviceAddress  = v.missRegion.deviceAddress + v.missRegion.size;

        std::vector<std::byte> handles(groupCount * handleSize);
        vkCheck(pvkGetRayTracingShaderGroupHandles(dev, v.pipeline, 0, groupCount, handles.size(), handles.data()),
                "get shader group handles");
        auto handle = [&](uint32_t i) { return handles.data() + i * handleSize; };

        std::byte* table = static_cast<std::byte*>(v.sbt.mapped) + (base - raw);
        std::byte* miss  = table + v.rgenRegion.size;
        std::byte* hit   = miss + v.missRegion.size;
        std::memcpy(table, handle(0), handleSize);                          // ray-gen
        std::memcpy(miss, handle(1), handleSize);                           // miss
        std::memcpy(miss + v.missRegion.stride, handle(2), handleSize);     // shadow miss
        std::memcpy(hit, handle(3), handleSize);                            // hit group
    }
}

// =============================================================================
//  Denoiser (edge-avoiding a-trous, SVGF-style) -- compute pipeline
// =============================================================================
void SceneEditor::createDenoiser() {
    // Layout matches shaders/atrous.comp:
    //   0 colorIn, 1 colorOut, 2 guide, 3 outLDR, 4 albedo -- all storage images.
    constexpr uint32_t bindingCount = 5;
    std::array<VkDescriptorSetLayoutBinding, bindingCount> bindings{};
    for (uint32_t i = 0; i < bindingCount; ++i)
        bindings[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = bindingCount;
    lci.pBindings    = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &denoiseLayout), "denoise set layout");

    std::array<VkDescriptorSetLayout, 3> layouts;
    layouts.fill(denoiseLayout);
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = descPool;
    ai.descriptorSetCount = static_cast<uint32_t>(denoiseSets.size());
    ai.pSetLayouts        = layouts.data();
    vkCheck(vkAllocateDescriptorSets(dev, &ai, denoiseSets.data()), "allocate denoise sets");

    // Ping-pong wiring. The first pass reads the accumulation buffer (alpha =
    // per-pixel sample count); guide, albedo and the display image are shared.
    const RendererView& pt = views[VIEW_PT];
    const std::array<VkImageView, 3> inputs { pt.accum.view, ping.view, pong.view };
    const std::array<VkImageView, 3> outputs{ ping.view,     pong.view, ping.view };

    FixedVector<VkDescriptorImageInfo, 3 * bindingCount> infos;   // pointer-stable, see createDescriptors
    FixedVector<VkWriteDescriptorSet,  3 * bindingCount> writes;
    for (std::size_t s = 0; s < denoiseSets.size(); ++s) {
        const std::array<VkImageView, bindingCount> bound{ inputs[s], outputs[s], guide.view, pt.output.view, albedo.view };
        for (uint32_t b = 0; b < bindingCount; ++b) {
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet          = denoiseSets[s];
            w.dstBinding      = b;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo      = &infos.push_back({ VK_NULL_HANDLE, bound[b], VK_IMAGE_LAYOUT_GENERAL });
            writes.push_back(w);
        }
    }
    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DenoisePC) };
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &denoiseLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pc;
    vkCheck(vkCreatePipelineLayout(dev, &pli, nullptr, &denoisePipeLayout), "denoise pipeline layout");

    ShaderModules shaders{ dev };
    VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage        = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = shaders.add(loadShader("shaders/atrous.comp.spv"));
    cpi.stage.pName  = "main";
    cpi.layout       = denoisePipeLayout;
    vkCheck(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &denoisePipeline), "denoise pipeline");
}
