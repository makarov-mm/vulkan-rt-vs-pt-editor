#pragma once
#include <cstdint>
#include <chrono>
#include <utility>
#include <vector>

#include "platform.h"
#include "config.h"
#include "cxx26.h"
#include "vec3.h"
#include "mat3.h"
#include "mat4.h"
#include "gpu_types.h"
#include "scene_object.h"
#include "editor_ui.h"
#include "buffer.h"
#include "mesh_info.h"

// =============================================================================
//  Scene editor
// =============================================================================
class SceneEditor {
public:
    // The editor owns every Vulkan handle it creates; copying or moving it
    // would double-destroy them in cleanup(). C++26 attaches the reason to
    // the deletion itself (P2573); until MSVC ships it, the macro degrades
    // to a plain = delete.
    SceneEditor() = default;
    SceneEditor(const SceneEditor&)            = CXX26_DELETED("SceneEditor owns raw Vulkan handles; copying would double-destroy them");
    SceneEditor& operator=(const SceneEditor&) = CXX26_DELETED("SceneEditor owns raw Vulkan handles; copying would double-destroy them");
public:
    void run(HINSTANCE hInst);

private:
    // ---- Win32 ----
    HWND hwnd     = nullptr;   // main window (hosts the toolbar)
    HWND viewHwnd = nullptr;   // child window the Vulkan swapchain renders into
    HWND hToolBtn[3] = {};     // select / move / rotate, for repaint on mode change
    bool quit = false;

    // ---- Toolbar icon bitmaps (PNG via GDI+; drawIcon is the fallback) ----
    static const int ICON_COUNT = 13;
    Gdiplus::Bitmap* iconBmp[ICON_COUNT] = {};

    static int iconIndex(int id);

    // ---- Interactive orbit camera (RMB drag + wheel) ----
    float camYaw      = 2.2f;
    float camPitch    = 0.42f;
    float camDist     = 15.0f;
    bool  orbiting    = false;
    POINT lastMouse{};
    bool  cameraDirty = true;   // forces an accumulation reset
    uint32_t accumFrame = 0;    // resets on camera move / scene edit
    uint32_t totalFrame = 0;    // never resets (drives RNG decorrelation)

    // ---- Editor tool state ----
    EditMode mode = MODE_SELECT;
    bool  lmbDown   = false;
    bool  dragMoved = false;
    POINT downMouse{};

    // Marquee (rubber band) in viewport pixels.
    bool  marqueeOn = false;
    POINT mq0{}, mq1{};

    // Move drag: horizontal on the grab plane, vertical while Shift is held.
    bool  movingObjs = false;
    float grabPlaneY = 0.0f;
    Vec3  grabPoint;
    Vec3  dragHoriz;
    float dragVert = 0.0f;
    std::vector<std::pair<int, Vec3>> dragOrig;   // index -> original position

    bool  rotatingObjs = false;

    // ---- Scene state ----
    std::vector<SceneObject> objects;   // editable objects (instance slot = FIXED_SLOTS + index)
    bool sceneDirty = true;             // instance/objdata buffers + TLAS need a rebuild
    float spawnAngle = 0.7f;            // golden-angle walk for new object placement

    // ---- Core Vulkan ----
    VkInstance       instance = VK_NULL_HANDLE;
    VkSurfaceKHR     surface  = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         dev      = VK_NULL_HANDLE;
    uint32_t         queueFamily = 0;
    VkQueue          queue    = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;

    // ---- Swapchain ----
    VkSwapchainKHR        swapchain = VK_NULL_HANDLE;
    VkFormat              swapFormat;
    VkExtent2D            swapExtent;
    std::vector<VkImage>  swapImages;

    // ---- Command / sync ----
    VkCommandPool   cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd     = VK_NULL_HANDLE;
    VkSemaphore     semImageAvailable = VK_NULL_HANDLE;
    std::vector<VkSemaphore> semRenderFinished;   // one per swapchain image (see drawFrame)
    VkFence         inFlight = VK_NULL_HANDLE;

    // ---- RT properties ----
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};

    // ---- Geometry (shared prototype meshes) ----
    Buffer vertexBuffer, indexBuffer, ubo;

    MeshInfo meshes[MESH_COUNT];

    std::vector<Vertex>   protoVerts;
    std::vector<uint32_t> protoIndices;

    // ---- Acceleration structures ----
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE; Buffer tlasBuffer;
    Buffer asScratch, instanceBuffer, objDataBuffer;
    VkAccelerationStructureGeometryKHR tlasGeom{};

    // ---- Two synchronized renderers over one shared scene --------------------
    // 0 = Whitted ray tracing (left half), 1 = Monte Carlo path tracing (right
    // half). Each view owns the state that differs per renderer: its pipeline
    // (different raygen shader), shader binding table, output image and the
    // accumulation/history pair. Everything else -- scene, camera, TLAS,
    // uniform buffer, toolbar -- is shared, which is what keeps the two halves
    // perfectly synchronized.
    static constexpr int VIEW_RT = 0, VIEW_PT = 1;
    struct RendererView {
        // Output storage image (rgba8, blitted into this view's half of the swapchain)
        VkImage        storageImage = VK_NULL_HANDLE;
        VkDeviceMemory storageMem   = VK_NULL_HANDLE;
        VkImageView    storageView  = VK_NULL_HANDLE;
        // Accumulation image (rgba32f, persists across frames)
        VkImage        accumImage = VK_NULL_HANDLE;
        VkDeviceMemory accumMem   = VK_NULL_HANDLE;
        VkImageView    accumView  = VK_NULL_HANDLE;
        // History image (rgba32f): xyz = previous primary-hit world pos, w = glass signature (PT)
        VkImage        histImage = VK_NULL_HANDLE;
        VkDeviceMemory histMem   = VK_NULL_HANDLE;
        VkImageView    histView  = VK_NULL_HANDLE;

        VkDescriptorSet descSet  = VK_NULL_HANDLE;
        VkPipeline      pipeline = VK_NULL_HANDLE;

        Buffer sbtBuffer;
        VkStridedDeviceAddressRegionKHR rgenRegion{}, missRegion{}, hitRegion{}, callRegion{};
    };
    RendererView views[2];

    // ---- Denoiser images (path-traced view only) ----
    // guide  (rgba32f): xyz = primary normal, w = depth (0 = sky, < 0 = glass)
    // albedo (rgba8)  : primary-hit albedo for SVGF demodulation
    // ping/pong (rgba32f): a-trous intermediate buffers
    VkImage        guideImage  = VK_NULL_HANDLE; VkDeviceMemory guideMem  = VK_NULL_HANDLE; VkImageView guideView  = VK_NULL_HANDLE;
    VkImage        albedoImage = VK_NULL_HANDLE; VkDeviceMemory albedoMem = VK_NULL_HANDLE; VkImageView albedoView = VK_NULL_HANDLE;
    VkImage        pingImage   = VK_NULL_HANDLE; VkDeviceMemory pingMem   = VK_NULL_HANDLE; VkImageView pingView   = VK_NULL_HANDLE;
    VkImage        pongImage   = VK_NULL_HANDLE; VkDeviceMemory pongMem   = VK_NULL_HANDLE; VkImageView pongView   = VK_NULL_HANDLE;

    // ---- Denoiser pipeline (a-trous compute) ----
    // Push constants must match the PC block in shaders/atrous.comp
    // (vec4 marquee is std430-aligned to 16 bytes, hence the padding).
    struct DenoisePC {
        int32_t sizeX, sizeY, step, finalPass;
        float   exposure;
        int32_t firstPass;
        float   _pad0, _pad1;
        float   marquee[4];
    };
    static_assert(sizeof(DenoisePC) == 48,
        "DenoisePC must match the push_constant block in atrous.comp (vec4 marquee is 16-aligned)");
    VkDescriptorSetLayout denoiseLayout     = VK_NULL_HANDLE;
    VkDescriptorSet       denoiseSets[3]    = {};   // 0: accum->ping, 1: ping->pong, 2: pong->ping
    VkPipelineLayout      denoisePipeLayout = VK_NULL_HANDLE;
    VkPipeline            denoisePipeline   = VK_NULL_HANDLE;
    bool                  denoiseEnabled    = true;   // 'D' toggles raw vs denoised

    // ---- Pipeline (layout shared by both views; per-view pipelines live in views[]) ----
    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      descPool   = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;

    std::chrono::high_resolution_clock::time_point startTime;

    // Both halves show the same scene, so interactions land in whichever half
    // the cursor is over. localX maps a viewport x into the per-view (WIDTH
    // wide) coordinate system, sticky to the half where the current drag
    // started so a drag crossing the seam cannot jump.
    int viewOffsetX = 0;
    int localX(int x) const {
        int lx = x - viewOffsetX;
        return lx < 0 ? 0 : (lx >= (int)WIDTH ? (int)WIDTH - 1 : lx);
    }

    static bool shiftHeld() { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }

    // -------------------------------------------------------------------------
    //  Win32: main window (toolbar) + viewport child (Vulkan surface)
    // -------------------------------------------------------------------------
    static LRESULT CALLBACK mainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    static LRESULT CALLBACK viewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    void onWheel(float delta);

    // -------------------------------------------------------------------------
    //  Viewport mouse: LMB = object operations (per tool), RMB = orbit camera
    // -------------------------------------------------------------------------
    void onLButtonDown(int x, int y, HWND h);

    void onMouseMove(int x, int y);

    void onLButtonUp(int x, int y);

    // -------------------------------------------------------------------------
    //  Window + toolbar
    // -------------------------------------------------------------------------
    void createWindow(HINSTANCE hInst);

    void createToolbar(HINSTANCE hInst);

    void onCommand(int id);

    // -------------------------------------------------------------------------
    //  Owner-drawn toolbar buttons: GDI icons, no image resources
    // -------------------------------------------------------------------------
    BOOL onDrawItem(DRAWITEMSTRUCT* d);

    void drawIcon(HDC dc, RECT rc, int id);

    // -------------------------------------------------------------------------
    //  Editor actions
    // -------------------------------------------------------------------------
    void markSceneDirty();

    void clearSelection();

    void addObject(int meshType);

    void deleteSelected();

    void deleteAll();

    // -------------------------------------------------------------------------
    //  Camera rays, picking and marquee selection
    // -------------------------------------------------------------------------
    void cameraBasis(Vec3& eye, Mat4& viewInv, Mat4& projInv);

    void rayThroughPixel(int px, int py, Vec3& eye, Vec3& dir);

    // World point -> viewport pixel. Returns false if behind the camera.
    bool projectToScreen(Vec3 w, float& sx, float& sy);

    int pickObject(int px, int py);

    // Select every object whose projected bounding circle touches the marquee.
    void marqueeSelect(bool additive);

    // -------------------------------------------------------------------------
    //  Vulkan setup
    // -------------------------------------------------------------------------
    void initVulkan();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCB(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void*);

    void createInstance();

    void createSurface();

    bool deviceSupportsRt(VkPhysicalDevice d);

    void pickPhysicalDevice();

    void createDevice();


    void createSwapchain();

    void createCommandResources();

    // ---- memory helpers ----
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps);

    void uploadToBuffer(Buffer& b, const void* data, VkDeviceSize size);

    VkDeviceAddress bufferAddress(VkBuffer buf);

    VkCommandBuffer beginOneShot();

    void endOneShot(VkCommandBuffer c);

    // -------------------------------------------------------------------------
    //  Prototype meshes: each shape type generated once in LOCAL space.
    //  Colour / material are per-instance (SSBO), so vertices carry only
    //  position + normal.
    // -------------------------------------------------------------------------
    uint32_t meshStartV = 0, meshStartI = 0;

    void beginMesh();

    void endMesh(int type);

    void pushVert(Vec3 p, Vec3 n);

    void genSphere(float radius, int stacks = 24, int slices = 48);

    // Flat-shaded triangle with an outward normal (oriented away from an interior point).
    void flatTri(Vec3 a, Vec3 b, Vec3 c, Vec3 interior);

    // Faceted brilliant-cut gem (flat facets -> sparkle).
    void genDiamond(float s, int N = 16);

    // Flat-shaded cube, extents +-s/2.
    void genCube(float s);

    // Square-based pyramid (base at y = 0, apex above).
    void genPyramid(float s, float h);

    // Cylinder with smooth (curved-normal) sides and flat caps, base at y = 0.
    void genCylinder(float r, float h, int N = 48);

    // Regular dodecahedron (12 flat pentagonal faces), s = circumradius.
    void genDodecahedron(float s);

    // Grid helper for the parametric shapes: positions via `pos`, normals from
    // central differences (radial fallback at degenerate poles).
    template <class Fn>
    void genGrid(int U, int Vr, bool outward, Fn pos) {
        uint32_t base = (uint32_t)protoVerts.size();
        int stride = Vr + 1;
        for (int i = 0; i <= U; ++i) {
            for (int j = 0; j <= Vr; ++j) {
                Vec3 p = pos(i, j);
                Vec3 du = pos(i + 1, j) - pos(i - 1, j);
                Vec3 dv = pos(i, j + 1) - pos(i, j - 1);
                Vec3 nn = du.cross(dv);
                float nl = std::sqrt(nn.dot(nn));
                nn = (nl > 1e-5f) ? nn * (1.0f / nl) : (p).normalize();
                if (outward && nn.dot(p) < 0.0f) nn = nn * -1.0f;
                pushVert(p, nn);
            }
        }
        for (int i = 0; i < U; ++i)
            for (int j = 0; j < Vr; ++j) {
                uint32_t a = base + i * stride + j, b = base + (i + 1) * stride + j;
                protoIndices.push_back(a);     protoIndices.push_back(b); protoIndices.push_back(a + 1);
                protoIndices.push_back(a + 1); protoIndices.push_back(b); protoIndices.push_back(b + 1);
            }
    }

    // Supertoroid: superellipse tube cross-section + integer twist (seamless ring).
    void genSupertoroid(int U = 120, int Vr = 60);

    static float superformula(float ang, float m, float n1, float n2, float n3);

    // Supershape: a curated, robust preset (soft 6-lobe), size-normalised.
    void genSupershape(int U = 120, int Vr = 60);

    void genFloor(float half);

    void createPrototypeMeshes();

    // The classic scene: central twisted supertoroid + six coloured glass shapes.
    void createDefaultScene();

    // -------------------------------------------------------------------------
    //  Acceleration structures: one BLAS per mesh (built once), TLAS with one
    //  instance per scene object (rebuilt only when the scene changes).
    // -------------------------------------------------------------------------
    void createAccelStructures();

    // Row-major 3x4 instance transform from column-major rotation, scale and translation.
    static void fillTransform(VkTransformMatrixKHR& t, const Mat3& R, float s, Vec3 T);

    // Rewrite the instance + per-object shading buffers and rebuild the TLAS.
    // Called only when the scene actually changes (add / delete / move / rotate / select).
    void updateSceneBuffers();

    void rebuildTlas(uint32_t instanceCount);

    // -------------------------------------------------------------------------
    //  Storage image the ray tracer writes into
    // -------------------------------------------------------------------------
    void createStorageImage();

    void createUniformBuffer();

    // -------------------------------------------------------------------------
    //  Descriptors
    // -------------------------------------------------------------------------
    void createDescriptors();

    // -------------------------------------------------------------------------
    //  Ray tracing pipeline
    // -------------------------------------------------------------------------
    VkShaderModule loadShader(const char* path);

    void createRayTracingPipeline();

    // -------------------------------------------------------------------------
    //  Shader binding table
    // -------------------------------------------------------------------------
    void createShaderBindingTable();

    // -------------------------------------------------------------------------
    //  Denoiser (edge-avoiding a-trous, SVGF-style)
    // -------------------------------------------------------------------------
    void createDenoiser();

    // -------------------------------------------------------------------------
    //  Per-frame
    // -------------------------------------------------------------------------
    void updateUniforms();

    void imageBarrier(VkCommandBuffer c, VkImage img,
        VkImageLayout oldL, VkImageLayout newL,
        VkAccessFlags srcA, VkAccessFlags dstA,
        VkPipelineStageFlags srcS, VkPipelineStageFlags dstS);

    void drawFrame();

    void mainLoop();

    // -------------------------------------------------------------------------
    //  Teardown
    // -------------------------------------------------------------------------
    void destroyBuffer(Buffer& b);

    void cleanup();
};
