// Ray tracing renderer: geometry, acceleration structures, images,
// descriptors, pipelines, shader binding tables, denoiser and per-frame
// command recording.
#include "renderer.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <print>
#include <vector>

namespace {
// Synchronization scopes used below (see Access in vk_utils.h).
constexpr Access RT_STORAGE_WRITE     { VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
constexpr Access RT_STORAGE_READ_WRITE{ VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
constexpr Access RT_AS_READ           { VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR };
constexpr Access CS_STORAGE_WRITE     { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
constexpr Access CS_STORAGE_READ      { VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT };
constexpr Access CS_STORAGE_READ_WRITE{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
constexpr Access AS_BUILD_WRITE       { VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR };
constexpr Access AS_BUILD_INPUT_READ  { VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT };
constexpr Access COPY_WRITE           { VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
constexpr Access TRANSFER_READ        { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };

// Row-major 3x4 instance transform from a column-major rotation, uniform scale and translation.
VkTransformMatrixKHR instanceTransform(const Mat3& r, float s, const Vec3& t) noexcept {
    const Vec3 c0 = r.c0 * s, c1 = r.c1 * s, c2 = r.c2 * s;
    return { { { c0.x, c1.x, c2.x, t.x },
               { c0.y, c1.y, c2.y, t.y },
               { c0.z, c1.z, c2.z, t.z } } };
}

// Destroys shader modules when pipeline creation is done (or throws).
class ShaderModules {
public:
    explicit ShaderModules(const VulkanContext& vk) noexcept : vk_(vk) {}
    ~ShaderModules() { for (VkShaderModule m : modules_) vkDestroyShaderModule(vk_.device(), m, nullptr); }
    ShaderModules(const ShaderModules&) = CXX26_DELETED("owns the shader modules");

    VkShaderModule load(const std::filesystem::path& path) { return modules_.emplace_back(vk_.loadShader(path)); }

private:
    const VulkanContext&        vk_;
    std::vector<VkShaderModule> modules_;
};

VkPipelineShaderStageCreateInfo shaderStage(VkShaderStageFlagBits stage, VkShaderModule shader) noexcept {
    return { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = stage, .module = shader, .pName = "main" };
}
} // namespace

Renderer::Renderer(VulkanContext& vk, const PrototypeMeshes& prototypes, const std::filesystem::path& shaderDir) : vk_(vk) {
    try {
        createGeometry(prototypes);
        createAccelStructures(prototypes);
        createImages();
        createDescriptors();
        createRayTracingPipelines(shaderDir);
        createShaderBindingTables();
        createDenoiser(shaderDir);
    } catch (...) {
        destroy();
        throw;
    }
}

Renderer::~Renderer() {
    destroy();
}

void Renderer::destroy() noexcept {
    const VkDevice dev = vk_.device();
    vkDeviceWaitIdle(dev);

    for (View& v : views_) {
        vk_.destroy(v.sbt);
        if (v.pipeline) vkDestroyPipeline(dev, v.pipeline, nullptr);
        vk_.destroy(v.output);
        vk_.destroy(v.accum);
        vk_.destroy(v.history);
        v = {};
    }
    if (denoisePipeline_)  vkDestroyPipeline(dev, denoisePipeline_, nullptr);
    if (denoiseLayout_)    vkDestroyPipelineLayout(dev, denoiseLayout_, nullptr);
    if (denoiseSetLayout_) vkDestroyDescriptorSetLayout(dev, denoiseSetLayout_, nullptr);
    if (pipelineLayout_)   vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
    if (descPool_)         vkDestroyDescriptorPool(dev, descPool_, nullptr);   // frees all sets
    if (descLayout_)       vkDestroyDescriptorSetLayout(dev, descLayout_, nullptr);
    denoisePipeline_  = VK_NULL_HANDLE;
    denoiseLayout_    = VK_NULL_HANDLE;
    denoiseSetLayout_ = VK_NULL_HANDLE;
    pipelineLayout_   = VK_NULL_HANDLE;
    descPool_         = VK_NULL_HANDLE;
    descLayout_       = VK_NULL_HANDLE;
    for (Image* img : { &guide_, &albedo_, &ping_, &pong_ }) vk_.destroy(*img);

    if (tlas_) vk_.rt().destroyAccelerationStructure(dev, tlas_, nullptr);
    tlas_ = VK_NULL_HANDLE;
    vk_.destroy(tlasBuffer_);
    for (Blas& b : blas_) {
        if (b.handle) vk_.rt().destroyAccelerationStructure(dev, b.handle, nullptr);
        vk_.destroy(b.buffer);
        b = {};
    }
    for (Buffer* b : { &scratch_, &instanceBuffer_, &objectBuffer_, &ubo_, &vertexBuffer_, &indexBuffer_ })
        vk_.destroy(*b);
}

// =============================================================================
//  Geometry and acceleration structures: one BLAS per mesh (built once), one
//  TLAS with an instance per scene object (rebuilt in the frame command
//  buffer on edits).
// =============================================================================
void Renderer::createGeometry(const PrototypeMeshes& prototypes) {
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |                                    // closest-hit normal lookup
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;   // BLAS build input
    vertexBuffer_ = vk_.createDeviceBuffer(std::as_bytes(std::span(prototypes.geometry.vertices())), usage);
    indexBuffer_  = vk_.createDeviceBuffer(std::as_bytes(std::span(prototypes.geometry.indices())),  usage);

    std::println("Prototype meshes: {} vertices, {} triangles across {} BLASes",
                 prototypes.geometry.vertices().size(), prototypes.geometry.indices().size() / 3, MESH_COUNT);
}

void Renderer::createAccelStructures(const PrototypeMeshes& prototypes) {
    const VkDevice        dev          = vk_.device();
    const VkDeviceAddress vtxAddr      = vk_.address(vertexBuffer_);
    const VkDeviceAddress idxAddr      = vk_.address(indexBuffer_);
    const VkDeviceSize    scratchAlign = vk_.asProperties().minAccelerationStructureScratchOffsetAlignment;

    // ---- size and create every BLAS ----
    std::array<VkAccelerationStructureGeometryKHR, MESH_COUNT>          geoms{};
    std::array<VkAccelerationStructureBuildGeometryInfoKHR, MESH_COUNT> infos{};
    std::array<VkAccelerationStructureBuildRangeInfoKHR, MESH_COUNT>    ranges{};
    std::array<VkDeviceSize, MESH_COUNT>                                scratchOffsets{};
    VkDeviceSize blasScratchTotal = 0;

    for (std::size_t m = 0; m < MESH_COUNT; ++m) {
        const MeshRange& mesh = prototypes.meshes[static_cast<MeshType>(m)];
        Blas&            blas = blas_[m];
        blas.firstIndex = mesh.firstIndex;

        // The stored indices address the whole shared vertex buffer, so the
        // vertex data starts at its base and maxVertex is a global index.
        geoms[m] = {
            .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry     = { .triangles = {
                .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                .vertexData   = { .deviceAddress = vtxAddr },
                .vertexStride = sizeof(Vertex),
                .maxVertex    = mesh.firstVert + mesh.vertCount - 1,
                .indexType    = VK_INDEX_TYPE_UINT32,
                .indexData    = { .deviceAddress = idxAddr + mesh.firstIndex * sizeof(uint32_t) },
            } },
            .flags        = VK_GEOMETRY_OPAQUE_BIT_KHR,
        };
        infos[m] = {
            .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,   // static, built once
            .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1,
            .pGeometries   = &geoms[m],
        };
        ranges[m] = { .primitiveCount = mesh.indexCount / 3 };

        VkAccelerationStructureBuildSizesInfoKHR sizes{ .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vk_.rt().getAccelerationStructureBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                    &infos[m], &ranges[m].primitiveCount, &sizes);

        blas.buffer = vk_.createBuffer(sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
        const VkAccelerationStructureCreateInfoKHR ci{
            .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = blas.buffer.buf,
            .size   = sizes.accelerationStructureSize,
            .type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        };
        vkCheck(vk_.rt().createAccelerationStructure(dev, &ci, nullptr, &blas.handle), "create BLAS");

        // Each BLAS gets its own scratch slice, so all of them build in one call.
        scratchOffsets[m] = blasScratchTotal;
        blasScratchTotal += alignUp(sizes.buildScratchSize, scratchAlign);
    }

    // ---- TLAS sized for the maximum instance count ----
    instanceBuffer_ = vk_.createHostBuffer(MAX_INSTANCES * sizeof(VkAccelerationStructureInstanceKHR),
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    objectBuffer_   = vk_.createHostBuffer(MAX_INSTANCES * sizeof(GpuObj), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    tlasGeometry_ = {
        .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry     = { .instances = {
            .sType           = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .arrayOfPointers = VK_FALSE,
            .data            = { .deviceAddress = vk_.address(instanceBuffer_) },
        } },
        .flags        = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };
    const VkAccelerationStructureBuildGeometryInfoKHR tlasInfo{
        .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
        .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries   = &tlasGeometry_,
    };
    const uint32_t maxInstances = MAX_INSTANCES;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{ .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vk_.rt().getAccelerationStructureBuildSizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &tlasInfo, &maxInstances, &tlasSizes);

    tlasBuffer_ = vk_.createBuffer(tlasSizes.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
    const VkAccelerationStructureCreateInfoKHR tlasCi{
        .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = tlasBuffer_.buf,
        .size   = tlasSizes.accelerationStructureSize,
        .type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    };
    vkCheck(vk_.rt().createAccelerationStructure(dev, &tlasCi, nullptr, &tlas_), "create TLAS");

    // ---- one scratch buffer: BLAS slices at startup, reused by every TLAS rebuild ----
    // Over-allocated by one alignment unit so the start can be aligned.
    scratch_ = vk_.createBuffer(std::max(blasScratchTotal, tlasSizes.buildScratchSize) + scratchAlign,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT });
    scratchAddress_ = alignUp(vk_.address(scratch_), scratchAlign);

    // ---- build every BLAS in a single command ----
    std::array<const VkAccelerationStructureBuildRangeInfoKHR*, MESH_COUNT> rangePtrs{};
    for (std::size_t m = 0; m < MESH_COUNT; ++m) {
        infos[m].dstAccelerationStructure  = blas_[m].handle;
        infos[m].scratchData.deviceAddress = scratchAddress_ + scratchOffsets[m];
        rangePtrs[m] = &ranges[m];
    }
    vk_.submitAndWait([&](VkCommandBuffer c) {
        // The staged vertex / index uploads (earlier submissions) -> build inputs.
        Barriers().memory(COPY_WRITE, AS_BUILD_INPUT_READ).record(c);
        vk_.rt().cmdBuildAccelerationStructures(c, static_cast<uint32_t>(MESH_COUNT), infos.data(), rangePtrs.data());
    });

    for (Blas& b : blas_) {
        const VkAccelerationStructureDeviceAddressInfoKHR ai{
            .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = b.handle,
        };
        b.address = vk_.rt().getAccelerationStructureDeviceAddress(dev, &ai);
    }
}

// =============================================================================
//  Images, uniform buffer, descriptors
// =============================================================================
void Renderer::createImages() {
    constexpr VkFormat HDR = VK_FORMAT_R32G32B32A32_SFLOAT;
    constexpr VkFormat LDR = VK_FORMAT_R8G8B8A8_UNORM;

    for (View& v : views_) {
        v.output  = vk_.createImage(extent(), LDR, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        v.accum   = vk_.createImage(extent(), HDR, VK_IMAGE_USAGE_STORAGE_BIT);
        v.history = vk_.createImage(extent(), HDR, VK_IMAGE_USAGE_STORAGE_BIT);
    }
    guide_  = vk_.createImage(extent(), HDR, VK_IMAGE_USAGE_STORAGE_BIT);
    albedo_ = vk_.createImage(extent(), LDR, VK_IMAGE_USAGE_STORAGE_BIT);
    ping_   = vk_.createImage(extent(), HDR, VK_IMAGE_USAGE_STORAGE_BIT);
    pong_   = vk_.createImage(extent(), HDR, VK_IMAGE_USAGE_STORAGE_BIT);

    // One-time transition to GENERAL; these images stay in GENERAL for good.
    // (The output images are transitioned every frame around the blit.)
    vk_.submitAndWait([&](VkCommandBuffer c) {
        constexpr Access firstUse{ VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT };
        Barriers b;
        for (const View& v : views_)
            for (const Image* img : { &v.accum, &v.history })
                b.image(img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, {}, firstUse);
        for (const Image* img : { &guide_, &albedo_, &ping_, &pong_ })
            b.image(img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, {}, firstUse);
        b.record(c);
    });
}

void Renderer::createDescriptors() {
    ubo_ = vk_.createHostBuffer(sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const VkDevice dev = vk_.device();

    // One layout serves both views. The Whitted ray-gen shader simply does not
    // declare bindings 8/9 (guide/albedo), which is legal.
    constexpr VkShaderStageFlags RGEN = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    constexpr VkShaderStageFlags CHIT = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    constexpr std::array<VkDescriptorSetLayoutBinding, 10> bindings{ {
        { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, RGEN, nullptr },   // TLAS
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // output
        { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             1, RGEN, nullptr },   // frame uniforms
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, CHIT, nullptr },   // vertices
        { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, CHIT, nullptr },   // indices
        { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // accumulation
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // history
        { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, CHIT, nullptr },   // object data
        { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // denoiser guide
        { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, RGEN, nullptr },   // denoiser albedo
    } };
    const VkDescriptorSetLayoutCreateInfo lci{
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings    = bindings.data(),
    };
    vkCheck(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &descLayout_), "vkCreateDescriptorSetLayout");

    // Per view: 5 storage images, 3 storage buffers, 1 UBO, 1 TLAS.
    // Denoiser: 3 sets x 5 storage images (allocated from the same pool).
        constexpr std::array<VkDescriptorPoolSize, 4> poolSizes{ {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VIEWS },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              VIEWS * 5 + DENOISE_SETS * 5 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             VIEWS },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             VIEWS * 3 },
    } };
    const VkDescriptorPoolCreateInfo pci{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = VIEWS + DENOISE_SETS,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes    = poolSizes.data(),
    };
    vkCheck(vkCreateDescriptorPool(dev, &pci, nullptr, &descPool_), "vkCreateDescriptorPool");

    std::array<VkDescriptorSetLayout, VIEWS> layouts;
    layouts.fill(descLayout_);
    std::array<VkDescriptorSet, VIEWS> sets{};
    const VkDescriptorSetAllocateInfo ai{
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = descPool_,
        .descriptorSetCount = VIEWS,
        .pSetLayouts        = layouts.data(),
    };
    vkCheck(vkAllocateDescriptorSets(dev, &ai, sets.data()), "vkAllocateDescriptorSets");
    for (std::size_t i = 0; i < VIEWS; ++i) views_[i].descSet = sets[i];

    // The TLAS is rebuilt in place on edits, so this handle stays valid for the whole run.
    const VkWriteDescriptorSetAccelerationStructureKHR asWrite{
        .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures    = &tlas_,
    };
    const VkDescriptorBufferInfo uboInfo{ ubo_.buf, 0, sizeof(FrameUniforms) };
    const VkDescriptorBufferInfo vtxInfo{ vertexBuffer_.buf, 0, VK_WHOLE_SIZE };
    const VkDescriptorBufferInfo idxInfo{ indexBuffer_.buf, 0, VK_WHOLE_SIZE };
    const VkDescriptorBufferInfo objInfo{ objectBuffer_.buf, 0, VK_WHOLE_SIZE };
    const VkDescriptorImageInfo  guideInfo { VK_NULL_HANDLE, guide_.view,  VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorImageInfo  albedoInfo{ VK_NULL_HANDLE, albedo_.view, VK_IMAGE_LAYOUT_GENERAL };

    // FixedVector (std::inplace_vector, C++26 P0843) guarantees by type that
    // elements never relocate, so a pushed element's address stays valid
    // until vkUpdateDescriptorSets consumes it.
    FixedVector<VkDescriptorImageInfo, VIEWS * 3>  imgInfos;
    FixedVector<VkWriteDescriptorSet,  VIEWS * 10> writes;

    for (const View& v : views_) {
        auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* img,
                         const VkDescriptorBufferInfo* buf, const void* next = nullptr) {
            writes.push_back({
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext           = next,
                .dstSet          = v.descSet,
                .dstBinding      = binding,
                .descriptorCount = 1,
                .descriptorType  = type,
                .pImageInfo      = img,
                .pBufferInfo     = buf,
            });
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
        write(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  &albedoInfo, nullptr);
    }
    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// =============================================================================
//  Ray tracing pipelines (one per view; they differ only in the ray-gen shader)
// =============================================================================
void Renderer::createRayTracingPipelines(const std::filesystem::path& shaderDir) {
    const VkPipelineLayoutCreateInfo lci{
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &descLayout_,
    };
    vkCheck(vkCreatePipelineLayout(vk_.device(), &lci, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");

    constexpr std::array<const char*, VIEWS> raygenFiles{
        "raygen_rt.rgen.spv",   // VIEW_RT: Whitted-style ray tracing
        "raygen_pt.rgen.spv",   // VIEW_PT: Monte Carlo path tracing
    };

    ShaderModules shaders(vk_);
    const VkShaderModule miss       = shaders.load(shaderDir / "miss.rmiss.spv");
    const VkShaderModule shadowMiss = shaders.load(shaderDir / "shadow.rmiss.spv");
    const VkShaderModule closestHit = shaders.load(shaderDir / "closesthit.rchit.spv");

    auto group = [](VkRayTracingShaderGroupTypeKHR type, uint32_t general, uint32_t hit) {
        return VkRayTracingShaderGroupCreateInfoKHR{
            .sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
            .type               = type,
            .generalShader      = general,
            .closestHitShader   = hit,
            .anyHitShader       = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR,
        };
    };
    // Groups: 0 ray-gen, 1 miss, 2 shadow miss, 3 triangle hit group (matches the SBT layout).
    const std::array<VkRayTracingShaderGroupCreateInfoKHR, 4> groups{
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, VK_SHADER_UNUSED_KHR),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1, VK_SHADER_UNUSED_KHR),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 2, VK_SHADER_UNUSED_KHR),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, 3),
    };

    // Both pipelines in one call, so the driver may compile them in parallel.
    std::array<std::array<VkPipelineShaderStageCreateInfo, 4>, VIEWS> stages{};
    std::array<VkRayTracingPipelineCreateInfoKHR, VIEWS>              infos{};
    for (std::size_t i = 0; i < VIEWS; ++i) {
        stages[i] = {
            shaderStage(VK_SHADER_STAGE_RAYGEN_BIT_KHR,      shaders.load(shaderDir / raygenFiles[i])),
            shaderStage(VK_SHADER_STAGE_MISS_BIT_KHR,        miss),
            shaderStage(VK_SHADER_STAGE_MISS_BIT_KHR,        shadowMiss),
            shaderStage(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, closestHit),
        };
        infos[i] = {
            .sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
            .stageCount                   = static_cast<uint32_t>(stages[i].size()),
            .pStages                      = stages[i].data(),
            .groupCount                   = static_cast<uint32_t>(groups.size()),
            .pGroups                      = groups.data(),
            .maxPipelineRayRecursionDepth = 1,   // every traceRay happens in ray-gen
            .layout                       = pipelineLayout_,
        };
    }
    std::array<VkPipeline, VIEWS> pipelines{};
    vkCheck(vk_.rt().createRayTracingPipelines(vk_.device(), VK_NULL_HANDLE, VK_NULL_HANDLE,
                                               static_cast<uint32_t>(infos.size()), infos.data(), nullptr,
                                               pipelines.data()),
            "vkCreateRayTracingPipelinesKHR");
    for (std::size_t i = 0; i < VIEWS; ++i) views_[i].pipeline = pipelines[i];
}

// =============================================================================
//  Shader binding tables (group handles are pipeline-specific: one per view)
// =============================================================================
void Renderer::createShaderBindingTables() {
    const auto&    props         = vk_.rtProperties();
    const uint32_t handleSize    = props.shaderGroupHandleSize;
    const uint32_t handleAligned = alignUp(handleSize, props.shaderGroupHandleAlignment);
    const uint32_t baseAlign     = props.shaderGroupBaseAlignment;
    constexpr uint32_t missCount = 2, hitCount = 1, groupCount = 1 + missCount + hitCount;

    for (View& v : views_) {
        // Each region must start baseAlign-aligned; for ray-gen, size == stride.
        v.rgenRegion.stride = alignUp(handleAligned, baseAlign);
        v.rgenRegion.size   = v.rgenRegion.stride;
        v.missRegion.stride = handleAligned;
        v.missRegion.size   = alignUp(missCount * handleAligned, baseAlign);
        v.hitRegion.stride  = handleAligned;
        v.hitRegion.size    = alignUp(hitCount * handleAligned, baseAlign);
        const VkDeviceSize tableSize = v.rgenRegion.size + v.missRegion.size + v.hitRegion.size;

        // Over-allocate by baseAlign so the table start itself can be aligned.
        v.sbt = vk_.createHostBuffer(tableSize + baseAlign, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR);
        const VkDeviceAddress raw  = vk_.address(v.sbt);
        const VkDeviceAddress base = alignUp<VkDeviceAddress>(raw, baseAlign);
        v.rgenRegion.deviceAddress = base;
        v.missRegion.deviceAddress = base + v.rgenRegion.size;
        v.hitRegion.deviceAddress  = v.missRegion.deviceAddress + v.missRegion.size;

        std::vector<std::byte> handles(groupCount * handleSize);
        vkCheck(vk_.rt().getRayTracingShaderGroupHandles(vk_.device(), v.pipeline, 0, groupCount, handles.size(), handles.data()),
                "vkGetRayTracingShaderGroupHandlesKHR");
        auto handle = [&](uint32_t i) { return handles.data() + i * handleSize; };

        std::byte* table = static_cast<std::byte*>(v.sbt.mapped) + (base - raw);
        std::byte* miss  = table + v.rgenRegion.size;
        std::byte* hit   = miss + v.missRegion.size;
        std::memcpy(table, handle(0), handleSize);                        // ray-gen
        std::memcpy(miss, handle(1), handleSize);                         // miss
        std::memcpy(miss + v.missRegion.stride, handle(2), handleSize);   // shadow miss
        std::memcpy(hit, handle(3), handleSize);                          // hit group
    }
}

// =============================================================================
//  Denoiser (edge-avoiding a-trous, SVGF-style) -- compute pipeline
// =============================================================================
void Renderer::createDenoiser(const std::filesystem::path& shaderDir) {
    const VkDevice dev = vk_.device();

    // Layout matches shaders/atrous.comp:
    //   0 colorIn, 1 colorOut, 2 guide, 3 outLDR, 4 albedo -- all storage images.
    constexpr uint32_t bindingCount = 5;
    std::array<VkDescriptorSetLayoutBinding, bindingCount> bindings{};
    for (uint32_t i = 0; i < bindingCount; ++i)
        bindings[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

    const VkDescriptorSetLayoutCreateInfo lci{
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = bindingCount,
        .pBindings    = bindings.data(),
    };
    vkCheck(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &denoiseSetLayout_), "vkCreateDescriptorSetLayout (denoise)");

    std::array<VkDescriptorSetLayout, DENOISE_SETS> layouts;
    layouts.fill(denoiseSetLayout_);
    const VkDescriptorSetAllocateInfo ai{
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = descPool_,
        .descriptorSetCount = static_cast<uint32_t>(layouts.size()),
        .pSetLayouts        = layouts.data(),
    };
    vkCheck(vkAllocateDescriptorSets(dev, &ai, denoiseSets_.data()), "vkAllocateDescriptorSets (denoise)");

    // Ping-pong wiring. The first pass reads the accumulation buffer (alpha =
    // per-pixel sample count); guide, albedo and the display image are shared.
    const View& pt = views_[VIEW_PT];
    const std::array<VkImageView, 3> inputs { pt.accum.view, ping_.view, pong_.view };
    const std::array<VkImageView, 3> outputs{ ping_.view,    pong_.view, ping_.view };

    FixedVector<VkDescriptorImageInfo, 3 * bindingCount> infos;   // pointer-stable, see createDescriptors
    FixedVector<VkWriteDescriptorSet,  3 * bindingCount> writes;
    for (std::size_t s = 0; s < denoiseSets_.size(); ++s) {
        const std::array<VkImageView, bindingCount> bound{ inputs[s], outputs[s], guide_.view, pt.output.view, albedo_.view };
        for (uint32_t b = 0; b < bindingCount; ++b) {
            writes.push_back({
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = denoiseSets_[s],
                .dstBinding      = b,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo      = &infos.push_back({ VK_NULL_HANDLE, bound[b], VK_IMAGE_LAYOUT_GENERAL }),
            });
        }
    }
    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DenoisePC) };
    const VkPipelineLayoutCreateInfo pli{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &denoiseSetLayout_,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pc,
    };
    vkCheck(vkCreatePipelineLayout(dev, &pli, nullptr, &denoiseLayout_), "vkCreatePipelineLayout (denoise)");

    ShaderModules shaders(vk_);
    const VkComputePipelineCreateInfo cpi{
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = shaderStage(VK_SHADER_STAGE_COMPUTE_BIT, shaders.load(shaderDir / "atrous.comp.spv")),
        .layout = denoiseLayout_,
    };
    vkCheck(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &denoisePipeline_), "vkCreateComputePipelines");
}

// =============================================================================
//  Frame
// =============================================================================
void Renderer::render(VkCommandBuffer c, const FrameDesc& frame) {
    writeUniforms(frame);
    if (frame.sceneChanged || !tlasBuilt_) {
        writeInstances(frame.objects);
        recordTlasBuild(c);
        tlasBuilt_ = true;
    }
    recordTrace(c);
    if (frame.denoise) recordDenoise(c, frame.marquee);
    recordOutputsReady(c, frame.denoise);
}

void Renderer::writeUniforms(const FrameDesc& frame) {
    // Restart accumulation when the camera or scene changed; otherwise keep adding samples.
    accumFrame_   = (frame.restartAccumulation || !accumulating_) ? 0 : accumFrame_ + 1;
    accumulating_ = true;
    ++totalFrame_;   // always advances, so the RNG keeps moving even during motion

    const FrameUniforms data{
        .viewInverse = frame.camera.viewInverse(),
        .projInverse = OrbitCamera::projInverse(),
        .light       = { LIGHT_POS.x, LIGHT_POS.y, LIGHT_POS.z, LIGHT_RADIUS },
        .lightColor  = { LIGHT_COLOR.x, LIGHT_COLOR.y, LIGHT_COLOR.z, LIGHT_EMISSION },
        .frame       = { accumFrame_, totalFrame_, SAMPLES_PER_FRAME, MAX_PATH_LENGTH },
        .marquee     = frame.marquee,
    };
    std::memcpy(ubo_.mapped, &data, sizeof(data));
}

// Scene -> instances. Only called when the scene actually changed (add /
// delete / move / rotate / select); the BLASes are never touched.
void Renderer::writeInstances(std::span<const SceneObject> objects) {
    assert(FIXED_SLOTS + objects.size() <= MAX_INSTANCES && "the editor caps the object count");
    objects = objects.first(std::min<std::size_t>(objects.size(), MAX_INSTANCES - FIXED_SLOTS));

    auto* inst = static_cast<VkAccelerationStructureInstanceKHR*>(instanceBuffer_.mapped);
    auto* obj  = static_cast<GpuObj*>(objectBuffer_.mapped);

    auto write = [&](uint32_t slot, MeshType type, const Mat3& rot, float scale, const Vec3& pos,
                     const Vec3& color, bool selected, Material material, float reflectivity) {
        const Blas& blas = blas_[std::to_underlying(type)];
        inst[slot] = {
            .transform                              = instanceTransform(rot, scale, pos),
            .instanceCustomIndex                    = slot,
            .mask                                   = 0xFF,
            .instanceShaderBindingTableRecordOffset = 0,
            .flags                                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
            .accelerationStructureReference         = blas.address,
        };
        obj[slot] = {
            .color  = { color.x, color.y, color.z, selected ? 1.0f : 0.0f },
            .params = { static_cast<float>(std::to_underlying(material)), reflectivity, 0.0f, 0.0f },
            .mesh   = { blas.firstIndex, 0, 0, 0 },
        };
    };

    // Slot 0: floor (the checker pattern comes from the shader); low gloss keeps the checker contrast.
    write(0, MeshType::Floor, {}, 1.0f, {}, { 0.8f, 0.8f, 0.8f }, false, Material::Floor, 0.18f);

    // Slot 1: the visible emissive area light (the sphere prototype scaled to LIGHT_RADIUS).
    write(1, MeshType::Sphere, {}, LIGHT_RADIUS / SPHERE_RADIUS, LIGHT_POS, LIGHT_COLOR, false, Material::Emissive, 0.0f);

    // Editable objects.
    for (uint32_t slot = FIXED_SLOTS; const SceneObject& o : objects)
        write(slot++, o.mesh, o.rot, o.scale, o.pos, o.color, o.selected, o.material, o.reflectivity);

    instanceCount_ = FIXED_SLOTS + static_cast<uint32_t>(objects.size());
}

void Renderer::recordTlasBuild(VkCommandBuffer c) {
    const VkAccelerationStructureBuildGeometryInfoKHR info{
        .sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type                     = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags                    = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
        .mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .dstAccelerationStructure = tlas_,
        .geometryCount            = 1,
        .pGeometries              = &tlasGeometry_,
        .scratchData              = { .deviceAddress = scratchAddress_ },
    };
    const VkAccelerationStructureBuildRangeInfoKHR range{ .primitiveCount = instanceCount_ };
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vk_.rt().cmdBuildAccelerationStructures(c, 1, &info, &ranges);

    // The ray-gen shaders of this frame traverse the freshly built TLAS.
    Barriers().memory(AS_BUILD_WRITE, RT_AS_READ).record(c);
}

// Cross-frame hazards (last frame's reads of what this frame overwrites) are
// covered by the host waiting for the previous frame before recording; the
// barriers here order the work inside one frame and make last frame's
// accumulation writes visible.
void Renderer::recordTrace(VkCommandBuffer c) {
    Barriers b;
    for (const View& v : views_)   // output images: fully overwritten by ray-gen
        b.image(v.output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, {}, RT_STORAGE_WRITE);
    b.memory(RT_STORAGE_WRITE, RT_STORAGE_READ_WRITE);   // accumulation + history: read back and rewritten
    b.record(c);

    // Whitted on the left, path traced on the right.
    for (const View& v : views_) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, v.pipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayout_, 0, 1, &v.descSet, 0, nullptr);
        vk_.rt().cmdTraceRays(c, &v.rgenRegion, &v.missRegion, &v.hitRegion, &v.callRegion, WIDTH, HEIGHT, 1);
    }
}

void Renderer::recordDenoise(VkCommandBuffer c, const Float4& marquee) {
    // Ray-gen writes -> compute: accumulation, guide and albedo are read; the
    // display image the ray-gen shader wrote is overwritten by the final pass.
    Barriers().memory(RT_STORAGE_WRITE, CS_STORAGE_READ_WRITE).record(c);

    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, denoisePipeline_);

    // Four a-trous iterations with growing step:
    // accum -> ping -> pong -> ping -> (pong + tone-mapped display image).
    struct Pass { int32_t step; uint32_t set; };
    constexpr std::array<Pass, 4> passes{ { { 1, 0 }, { 2, 1 }, { 4, 2 }, { 8, 1 } } };

    DenoisePC pc{ .marquee = marquee };   // re-composited, since the final pass overwrites the ray-gen overlay
    for (std::size_t i = 0; i < passes.size(); ++i) {
        const Pass& p = passes[i];
        pc.step      = p.step;
        pc.firstPass = (i == 0) ? 1 : 0;
        pc.finalPass = (i + 1 == passes.size()) ? 1 : 0;

        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, denoiseLayout_, 0, 1, &denoiseSets_[p.set], 0, nullptr);
        vkCmdPushConstants(c, denoiseLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(c, (WIDTH + 7) / 8, (HEIGHT + 7) / 8, 1);

        if (!pc.finalPass)   // this pass's output is the next pass's input
            Barriers().memory(CS_STORAGE_WRITE, CS_STORAGE_READ).record(c);
    }
}

// Output images -> transfer source for the blit into the swapchain image.
void Renderer::recordOutputsReady(VkCommandBuffer c, bool denoised) {
    Barriers b;
    for (std::size_t vi = 0; vi < VIEWS; ++vi) {
        // Written last by the final denoise pass (path-traced view) or by ray-gen.
        const Access writer = (denoised && vi == VIEW_PT) ? CS_STORAGE_WRITE : RT_STORAGE_WRITE;
        b.image(views_[vi].output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, writer, TRANSFER_READ);
    }
    b.record(c);
}
