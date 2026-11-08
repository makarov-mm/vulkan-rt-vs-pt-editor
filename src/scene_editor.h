#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "platform.h"
#include "camera.h"
#include "config.h"
#include "cxx26.h"
#include "editor_ui.h"
#include "gpu_types.h"
#include "mesh_builder.h"
#include "scene_object.h"
#include "vk_utils.h"

// =============================================================================
//  Scene editor: Win32 window + toolbar, editor state, and two hardware ray
//  tracing renderers (Whitted | path traced) over one shared scene.
//
//  Implementation is split by concern:
//    scene_editor.cpp   lifecycle, per-frame recording, teardown
//    editor_input.cpp   window procedures, toolbar, mouse/keyboard, picking
//    vulkan_setup.cpp   device, swapchain, resources, AS, pipelines, SBT
// =============================================================================
class SceneEditor {
public:
    SceneEditor() = default;
    ~SceneEditor();

    // The editor owns raw Vulkan and Win32 handles; a copy would destroy them twice.
    SceneEditor(const SceneEditor&)            = CXX26_DELETED("SceneEditor owns raw Vulkan handles; copying would double-destroy them");
    SceneEditor& operator=(const SceneEditor&) = CXX26_DELETED("SceneEditor owns raw Vulkan handles; copying would double-destroy them");

    void run(HINSTANCE hInst);

private:
    // ---- Types --------------------------------------------------------------
    struct MeshInfo : MeshRange {
        VkAccelerationStructureKHR blas     = VK_NULL_HANDLE;
        Buffer                     blasBuf;
        VkDeviceAddress            blasAddr = 0;
    };

    // One renderer: 0 = Whitted ray tracing (left), 1 = path tracing (right).
    // Everything else -- scene, camera, TLAS, UBO, toolbar -- is shared, which
    // keeps the two halves perfectly synchronized.
    static constexpr std::size_t VIEW_RT = 0, VIEW_PT = 1;
    struct RendererView {
        Image output;    // rgba8, blitted into this view's half of the swapchain
        Image accum;     // rgba32f, rgb = running mean, a = sample count
        Image history;   // rgba32f, xyz = previous primary hit, w = glass signature (PT)

        VkDescriptorSet descSet  = VK_NULL_HANDLE;
        VkPipeline      pipeline = VK_NULL_HANDLE;

        Buffer sbt;
        VkStridedDeviceAddressRegionKHR rgenRegion{}, missRegion{}, hitRegion{}, callRegion{};
    };

    // ---- Win32 --------------------------------------------------------------
    HWND hwnd     = nullptr;   // main window (hosts the toolbar)
    HWND viewHwnd = nullptr;   // child window the swapchain presents into
    std::array<HWND, 3> toolButtons{};   // select / move / rotate, repainted on mode change
    bool quit = false;

    static constexpr int ICON_COUNT = 13;   // 8 shapes + 3 tools + 2 delete buttons
    std::array<std::unique_ptr<Gdiplus::Bitmap>, ICON_COUNT> icons;   // nullptr -> GDI fallback

    // ---- Camera / accumulation ----------------------------------------------
    OrbitCamera camera;
    bool     orbiting   = false;
    POINT    lastMouse{};
    bool     resetAccum = true;   // restart temporal accumulation next frame
    uint32_t accumFrame = 0;      // resets on camera move / scene edit
    uint32_t totalFrame = 0;      // never resets (drives RNG decorrelation)

    // ---- Editor tool state --------------------------------------------------
    EditMode mode      = EditMode::Select;
    bool     lmbDown   = false;
    bool     dragMoved = false;
    POINT    downMouse{};
    int      viewOffsetX = 0;     // x of the half the current interaction started in

    bool  marqueeOn = false;      // rubber band, in view-local pixels
    POINT mq0{}, mq1{};

    bool  movingObjs = false;     // move drag: ground plane, vertical with Shift
    float grabPlaneY = 0.0f;
    Vec3  grabPoint;
    Vec3  dragHoriz;
    float dragVert   = 0.0f;
    std::vector<std::pair<std::size_t, Vec3>> dragOrig;   // object index -> original position

    bool rotatingObjs = false;

    // ---- Scene --------------------------------------------------------------
    std::vector<SceneObject> objects;   // editable objects; instance slot = FIXED_SLOTS + index
    bool  sceneDirty     = true;        // instances / object data / TLAS need a rebuild
    float spawnAngle     = 0.7f;        // golden-angle walk for new object placement
    bool  denoiseEnabled = true;        // 'D' toggles raw vs denoised path tracing

    // ---- Core Vulkan --------------------------------------------------------
    VkInstance               instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR             surface        = VK_NULL_HANDLE;
    VkPhysicalDevice         phys           = VK_NULL_HANDLE;
    VkDevice                 dev            = VK_NULL_HANDLE;
    uint32_t                 queueFamily    = 0;
    VkQueue                  queue          = VK_NULL_HANDLE;

    VkPhysicalDeviceMemoryProperties                   memProps{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR    rtProps{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};

    // ---- Swapchain ----------------------------------------------------------
    VkSwapchainKHR       swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR   swapFormat{};
    VkExtent2D           swapExtent{};
    std::vector<VkImage> swapImages;
    bool                 swapchainStale = false;   // out of date / suboptimal: recreate before the next frame

    // ---- Commands / sync (one frame in flight) ------------------------------
    VkCommandPool            cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer          cmd     = VK_NULL_HANDLE;
    VkSemaphore              semImageAvailable = VK_NULL_HANDLE;
    std::vector<VkSemaphore> semRenderFinished;   // one per swapchain image (see drawFrame)
    VkFence                  inFlight = VK_NULL_HANDLE;

    // ---- Geometry and acceleration structures -------------------------------
    std::array<MeshInfo, MESH_COUNT> meshes{};
    Buffer vertexBuffer, indexBuffer;   // device-local, shared by all BLASes
    Buffer instanceBuffer;              // host-visible, MAX_INSTANCES entries
    Buffer objDataBuffer;               // host-visible, GpuObj per instance
    Buffer ubo;                         // host-visible
    Buffer asScratch;
    VkDeviceAddress scratchAddr = 0;    // aligned to minAccelerationStructureScratchOffsetAlignment

    VkAccelerationStructureKHR         tlas = VK_NULL_HANDLE;
    Buffer                             tlasBuffer;
    VkAccelerationStructureGeometryKHR tlasGeom{};

    // ---- Renderers ----------------------------------------------------------
    std::array<RendererView, VIEWS> views{};
    VkDescriptorSetLayout descLayout     = VK_NULL_HANDLE;
    VkDescriptorPool      descPool       = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;

    // ---- Denoiser (path-traced view only, edge-avoiding a-trous) -------------
    Image guide;    // rgba32f: xyz = primary normal, w = depth (0 = sky, < 0 = glass)
    Image albedo;   // rgba8:   primary-hit albedo for demodulation
    Image ping, pong;   // rgba32f a-trous intermediates

    VkDescriptorSetLayout          denoiseLayout     = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 3> denoiseSets{};    // 0: accum->ping, 1: ping->pong, 2: pong->ping
    VkPipelineLayout               denoisePipeLayout = VK_NULL_HANDLE;
    VkPipeline                     denoisePipeline   = VK_NULL_HANDLE;

    std::chrono::steady_clock::time_point startTime;

    // =========================================================================
    //  editor_input.cpp
    // =========================================================================
    static LRESULT CALLBACK mainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK viewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    void createWindow(HINSTANCE hInst);
    void createToolbar(HINSTANCE hInst);
    void loadIcons();

    void onCommand(int id);
    void onKeyDown(WPARAM key, LPARAM flags);
    void onWheel(float steps);
    void onRButtonDown(int x, int y);
    void onLButtonDown(int x, int y);
    void onMouseMove(int x, int y);
    void onLButtonUp(int x, int y);
    void onCaptureLost();

    BOOL onDrawItem(const DRAWITEMSTRUCT& d);
    void drawFallbackIcon(HDC dc, const RECT& rc, int id) const;
    static int iconIndex(int id);

    // Maps a viewport x into the per-view (WIDTH wide) coordinates of the half
    // the current interaction started in, so a drag cannot jump at the seam.
    [[nodiscard]] int localX(int x) const noexcept;
    [[nodiscard]] static bool shiftHeld() noexcept { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }

    // Editor actions
    void markSceneDirty() noexcept;
    void clearSelection() noexcept;
    void addObject(MeshType type);
    void deleteSelected();
    void deleteAll();

    // Picking and marquee selection
    [[nodiscard]] Vec3 worldCenter(const SceneObject& o) const noexcept;
    [[nodiscard]] std::optional<std::size_t> pickObject(int px, int py) const noexcept;
    void marqueeSelect(bool additive);
    [[nodiscard]] Float4 marqueeRect() const noexcept;

    // =========================================================================
    //  vulkan_setup.cpp
    // =========================================================================
    void initVulkan();
    void createInstance();
    void createSurface();
    void pickPhysicalDevice();
    void createDevice();
    void createSwapchain();
    bool recreateSwapchain();   // false while the window is minimized
    void destroySwapchain();
    void createCommandResources();

    // First memory type allowed by typeBits that has all flags of the earliest
    // satisfiable entry in `preferences`.
    [[nodiscard]] uint32_t findMemoryType(uint32_t typeBits, std::initializer_list<VkMemoryPropertyFlags> preferences) const;
    [[nodiscard]] Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                      std::initializer_list<VkMemoryPropertyFlags> memPreferences);
    [[nodiscard]] Buffer createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage);   // prefers device-local (ReBAR)
    [[nodiscard]] Buffer createDeviceBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage);   // staged upload
    [[nodiscard]] Image  createImage(VkFormat format, VkImageUsageFlags usage);
    [[nodiscard]] VkDeviceAddress bufferAddress(const Buffer& b) const;
    void destroyBuffer(Buffer& b) noexcept;
    void destroyImage(Image& img) noexcept;

    [[nodiscard]] VkCommandBuffer beginOneShot();
    void submitOneShot(VkCommandBuffer c);   // submits, waits, frees

    void createPrototypeMeshes();
    void createDefaultScene();
    void createAccelStructures();
    void createImages();
    void createUniformBuffer();
    void createDescriptors();
    [[nodiscard]] VkShaderModule loadShader(const char* path) const;
    void createRayTracingPipelines();
    void createShaderBindingTables();
    void createDenoiser();

    // =========================================================================
    //  scene_editor.cpp
    // =========================================================================
    void mainLoop();
    void drawFrame();
    void updateUniforms();
    void writeSceneInstances();                 // host side: instance + object data buffers
    void recordTlasBuild(VkCommandBuffer c);    // device side: rebuild the TLAS from them
    void recordTrace(VkCommandBuffer c);
    void recordDenoise(VkCommandBuffer c);
    void recordBlitToSwapchain(VkCommandBuffer c, uint32_t imageIndex);
    void cleanup() noexcept;

    [[nodiscard]] MeshInfo&       mesh(MeshType t)       noexcept { return meshes[std::to_underlying(t)]; }
    [[nodiscard]] const MeshInfo& mesh(MeshType t) const noexcept { return meshes[std::to_underlying(t)]; }
};

// Records a single image memory barrier.
void imageBarrier(VkCommandBuffer c, VkImage img,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
