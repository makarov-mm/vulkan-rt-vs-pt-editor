#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include <vulkan/vulkan.h>

#include "camera.h"
#include "config.h"
#include "cxx26.h"
#include "gpu_types.h"
#include "scene.h"
#include "vk_context.h"

// Everything one frame of the renderer depends on.
struct FrameDesc {
    const OrbitCamera&           camera;
    std::span<const SceneObject> objects;
    Float4 marquee             = MARQUEE_OFF;   // editor rubber band, drawn over both views
    bool   sceneChanged        = false;         // rewrite the instances and rebuild the TLAS
    bool   restartAccumulation = false;         // camera moved or the scene was edited
    bool   denoise             = true;          // filter the path-traced view
};

// =============================================================================
//  The two hardware ray tracing renderers over one shared scene: Whitted ray
//  tracing (VIEW_RT, left) and Monte Carlo path tracing with an SVGF-style
//  a-trous denoiser (VIEW_PT, right). Scene, camera, TLAS and uniforms are
//  shared, which keeps the two halves perfectly synchronized.
//
//  One BLAS per prototype mesh, built once. Every scene object is a TLAS
//  instance with its own transform, colour and material, so add / delete /
//  move / rotate only rewrite the instance buffer and rebuild the TLAS.
// =============================================================================
class Renderer {
public:
    static constexpr std::size_t VIEW_RT = 0, VIEW_PT = 1;

    Renderer(VulkanContext& vk, const PrototypeMeshes& prototypes, const std::filesystem::path& shaderDir);
    ~Renderer();

    Renderer(const Renderer&)            = CXX26_DELETED("owns raw Vulkan handles; a copy would destroy them twice");
    Renderer& operator=(const Renderer&) = CXX26_DELETED("owns raw Vulkan handles; a copy would destroy them twice");

    // Writes this frame's host-visible data and records the frame into `c`.
    // The GPU must be done with the previous frame (one frame in flight).
    // Afterwards every output(view) is in TRANSFER_SRC_OPTIMAL, ready to be
    // blitted or copied.
    void render(VkCommandBuffer c, const FrameDesc& frame);

    [[nodiscard]] VkImage output(std::size_t view) const noexcept { return views_[view].output.image; }
    [[nodiscard]] static constexpr VkExtent2D extent() noexcept { return { WIDTH, HEIGHT }; }

private:
    // One renderer: its images, pipeline and shader binding table.
    struct View {
        Image output;    // rgba8, display-referred: blitted into this view's half of the window
        Image accum;     // rgba32f, rgb = running mean, a = sample count
        Image history;   // rgba32f, xyz = previous primary hit, w = glass signature (PT)

        VkDescriptorSet descSet  = VK_NULL_HANDLE;
        VkPipeline      pipeline = VK_NULL_HANDLE;

        Buffer sbt;
        VkStridedDeviceAddressRegionKHR rgenRegion{}, missRegion{}, hitRegion{}, callRegion{};
    };

    // One prototype mesh on the GPU.
    struct Blas {
        VkAccelerationStructureKHR handle     = VK_NULL_HANDLE;
        Buffer                     buffer;
        VkDeviceAddress            address    = 0;
        uint32_t                   firstIndex = 0;   // slice of the shared index buffer (closest-hit lookup)
    };

    // ---- Setup --------------------------------------------------------------
    void createGeometry(const PrototypeMeshes& prototypes);
    void createAccelStructures(const PrototypeMeshes& prototypes);
    void createImages();
    void createDescriptors();
    void createRayTracingPipelines(const std::filesystem::path& shaderDir);
    void createShaderBindingTables();
    void createDenoiser(const std::filesystem::path& shaderDir);
    void destroy() noexcept;   // idempotent; also runs when the constructor throws halfway

    // ---- Per frame ----------------------------------------------------------
    void writeUniforms(const FrameDesc& frame);
    void writeInstances(std::span<const SceneObject> objects);   // host side: instance + object data buffers
    void recordTlasBuild(VkCommandBuffer c);                      // device side: rebuild the TLAS from them
    void recordTrace(VkCommandBuffer c);
    void recordDenoise(VkCommandBuffer c, const Float4& marquee);
    void recordOutputsReady(VkCommandBuffer c, bool denoised);

    VulkanContext& vk_;

    // ---- Geometry and acceleration structures -------------------------------
    Buffer vertexBuffer_, indexBuffer_;      // device-local, shared by all BLASes
    std::array<Blas, MESH_COUNT> blas_{};    // indexed by MeshType
    Buffer instanceBuffer_;                  // host-visible, MAX_INSTANCES entries
    Buffer objectBuffer_;                    // host-visible, GpuObj per instance
    Buffer scratch_;                         // BLAS builds at startup, then every TLAS rebuild
    VkDeviceAddress scratchAddress_ = 0;     // aligned to minAccelerationStructureScratchOffsetAlignment

    VkAccelerationStructureKHR         tlas_ = VK_NULL_HANDLE;
    Buffer                             tlasBuffer_;
    VkAccelerationStructureGeometryKHR tlasGeometry_{};
    uint32_t                           instanceCount_ = 0;
    bool                               tlasBuilt_     = false;

    // ---- Frame uniforms and temporal accumulation -----------------------------
    Buffer   ubo_;                  // host-visible
    uint32_t accumFrame_   = 0;     // restarts on camera moves / scene edits
    uint32_t totalFrame_   = 0;     // never restarts (drives RNG decorrelation)
    bool     accumulating_ = false;

    // ---- Renderers ----------------------------------------------------------
    std::array<View, VIEWS> views_{};
    VkDescriptorSetLayout   descLayout_     = VK_NULL_HANDLE;
    VkDescriptorPool        descPool_       = VK_NULL_HANDLE;
    VkPipelineLayout        pipelineLayout_ = VK_NULL_HANDLE;

    // ---- Denoiser (path-traced view only, edge-avoiding a-trous) -------------
    Image guide_;           // rgba32f: xyz = primary normal, w = depth (0 = sky, < 0 = glass)
    Image albedo_;          // rgba8:   primary-hit albedo for demodulation
    Image ping_, pong_;     // rgba32f a-trous intermediates

    static constexpr uint32_t DENOISE_SETS = 3;   // 0: accum->ping, 1: ping->pong, 2: pong->ping
    VkDescriptorSetLayout                     denoiseSetLayout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, DENOISE_SETS> denoiseSets_{};
    VkPipelineLayout               denoiseLayout_    = VK_NULL_HANDLE;
    VkPipeline                     denoisePipeline_  = VK_NULL_HANDLE;
};
