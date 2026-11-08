// Lifecycle, per-frame command recording and teardown.
#include "scene_editor.h"

#include <cstring>
#include <ranges>

#include "rt_functions.h"

SceneEditor::~SceneEditor() {
    cleanup();
}

void SceneEditor::run(HINSTANCE hInst) {
    createWindow(hInst);
    initVulkan();
    mainLoop();
}

void SceneEditor::mainLoop() {
    MSG msg{};
    while (!quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;

        // Nothing is visible while minimized: sleep until a message arrives
        // instead of spinning on a zero-sized swapchain.
        if (IsIconic(hwnd)) { WaitMessage(); continue; }

        drawFrame();
    }
    vkCheck(vkDeviceWaitIdle(dev));
}

// =============================================================================
//  Frame
// =============================================================================
void SceneEditor::drawFrame() {
    if (swapchainStale && !recreateSwapchain()) return;

    vkCheck(vkWaitForFences(dev, 1, &inFlight, VK_TRUE, UINT64_MAX));

    uint32_t imageIndex = 0;
    const VkResult acquired = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX,
                                                    semImageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { swapchainStale = true; return; }
    if (acquired == VK_SUBOPTIMAL_KHR) swapchainStale = true;   // still usable; recreate after this frame
    else vkCheck(acquired, "vkAcquireNextImageKHR");

    // Reset only once we are committed to submitting, so an early return can
    // never leave the fence unsignalled (which would freeze the next frame).
    vkCheck(vkResetFences(dev, 1, &inFlight));

    // The previous frame has finished (fence), so the host-visible buffers
    // can be rewritten without racing the GPU.
    updateUniforms();
    const bool rebuildTlas = std::exchange(sceneDirty, false);
    if (rebuildTlas) writeSceneInstances();

    vkCheck(vkResetCommandBuffer(cmd, 0));
    const VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkCheck(vkBeginCommandBuffer(cmd, &bi));
    if (rebuildTlas) recordTlasBuild(cmd);
    recordTrace(cmd);
    if (denoiseEnabled) recordDenoise(cmd);
    recordBlitToSwapchain(cmd, imageIndex);
    vkCheck(vkEndCommandBuffer(cmd));

    // Ray tracing does not touch the swapchain image, so only the blit waits
    // for the acquire: tracing overlaps the presentation engine's latency.
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &semImageAvailable;
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &semRenderFinished[imageIndex];
    vkCheck(vkQueueSubmit(queue, 1, &si, inFlight), "vkQueueSubmit");

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &semRenderFinished[imageIndex];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &swapchain;
    pi.pImageIndices      = &imageIndex;
    const VkResult presented = vkQueuePresentKHR(queue, &pi);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) swapchainStale = true;
    else vkCheck(presented, "vkQueuePresentKHR");
}

void SceneEditor::updateUniforms() {
    // Restart accumulation when the camera or scene changed; otherwise keep adding samples.
    accumFrame = std::exchange(resetAccum, false) ? 0 : accumFrame + 1;
    ++totalFrame;   // always advances, so the RNG keeps moving even during motion

    const UBO data{
        .viewInverse = camera.viewInverse(),
        .projInverse = OrbitCamera::projInverse(),
        .lightPos    = { LIGHT_POS.x, LIGHT_POS.y, LIGHT_POS.z, LIGHT_RADIUS },
        .params      = { std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count(),
                         static_cast<float>(MAX_PATH_LENGTH), LIGHT_EMISSION,
                         static_cast<float>(SAMPLES_PER_FRAME) },
        .frame       = { accumFrame, totalFrame, 0, 0 },
        .marquee     = marqueeRect(),
    };
    std::memcpy(ubo.mapped, &data, sizeof(data));
}

// =============================================================================
//  Scene -> instances. Only called when the scene actually changed (add /
//  delete / move / rotate / select); the BLASes are never touched.
// =============================================================================
namespace {
// Row-major 3x4 instance transform from a column-major rotation, uniform scale and translation.
VkTransformMatrixKHR instanceTransform(const Mat3& r, float s, const Vec3& t) noexcept {
    const Vec3 c0 = r.c0 * s, c1 = r.c1 * s, c2 = r.c2 * s;
    return { { { c0.x, c1.x, c2.x, t.x },
               { c0.y, c1.y, c2.y, t.y },
               { c0.z, c1.z, c2.z, t.z } } };
}
} // namespace

void SceneEditor::writeSceneInstances() {
    auto* inst = static_cast<VkAccelerationStructureInstanceKHR*>(instanceBuffer.mapped);
    auto* obj  = static_cast<GpuObj*>(objDataBuffer.mapped);

    auto write = [&](uint32_t slot, MeshType type, const Mat3& rot, float scale, const Vec3& pos,
                     const Vec3& color, bool selected, Material material, float reflectivity) {
        VkAccelerationStructureInstanceKHR i{};
        i.transform                              = instanceTransform(rot, scale, pos);
        i.instanceCustomIndex                    = slot;
        i.mask                                   = 0xFF;
        i.flags                                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        i.accelerationStructureReference         = mesh(type).blasAddr;
        inst[slot] = i;

        obj[slot] = {
            .color  = { color.x, color.y, color.z, selected ? 1.0f : 0.0f },
            .params = { static_cast<float>(std::to_underlying(material)), reflectivity, 0.0f, 0.0f },
            .mesh   = { mesh(type).firstIndex, 0, 0, 0 },
        };
    };

    // Slot 0: floor (the checker pattern comes from the shader).
    // Low gloss keeps the checker contrast.
    write(0, MeshType::Floor, {}, 1.0f, {}, { 0.8f, 0.8f, 0.8f }, false, Material::Floor, 0.18f);

    // Slot 1: the visible emissive area light (the sphere prototype scaled to LIGHT_RADIUS).
    write(1, MeshType::Sphere, {}, LIGHT_RADIUS / SPHERE_RADIUS, LIGHT_POS,
          LIGHT_COLOR, false, Material::Emissive, 0.0f);

    // Editable objects.
    for (uint32_t slot = FIXED_SLOTS; const SceneObject& o : objects)
        write(slot++, o.mesh, o.rot, o.scale, o.pos, o.color, o.selected, o.material, o.reflectivity);
}

void SceneEditor::recordTlasBuild(VkCommandBuffer c) {
    VkAccelerationStructureBuildGeometryInfoKHR info{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    info.type                      = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags                     = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    info.mode                      = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.dstAccelerationStructure  = tlas;
    info.geometryCount             = 1;
    info.pGeometries               = &tlasGeom;
    info.scratchData.deviceAddress = scratchAddr;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = FIXED_SLOTS + static_cast<uint32_t>(objects.size());
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    pvkCmdBuildAccelerationStructures(c, 1, &info, &ranges);

    // The ray-gen shaders of this frame traverse the freshly built TLAS.
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    b.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 1, &b, 0, nullptr, 0, nullptr);
}

// =============================================================================
//  Recording
// =============================================================================
void SceneEditor::recordTrace(VkCommandBuffer c) {
    // Whitted on the left, path traced on the right.
    for (RendererView& v : views) {
        // Output image -> GENERAL for the ray-gen writes (fully overwritten).
        imageBarrier(c, v.output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     0, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

        // Accumulation and history are read this frame and were written last frame.
        for (const Image* img : { &v.accum, &v.history })
            imageBarrier(c, img->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, v.pipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayout,
                                0, 1, &v.descSet, 0, nullptr);
        pvkCmdTraceRays(c, &v.rgenRegion, &v.missRegion, &v.hitRegion, &v.callRegion, WIDTH, HEIGHT, 1);
    }
}

void SceneEditor::recordDenoise(VkCommandBuffer c) {
    const RendererView& pt = views[VIEW_PT];

    // Ray-gen writes (accum, guide, albedo) -> compute reads.
    for (VkImage img : { pt.accum.image, guide.image, albedo.image })
        imageBarrier(c, img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // The ray-gen shader wrote its raw image into the display image; the final
    // a-trous pass overwrites it (write-after-write).
    imageBarrier(c, pt.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, denoisePipeline);

    // Four a-trous iterations with growing step:
    // accum -> ping -> pong -> ping -> (pong + tone-mapped display image).
    struct Pass { int step; uint32_t set; const Image* out; };
    const std::array<Pass, 4> passes{ { { 1, 0, &ping }, { 2, 1, &pong }, { 4, 2, &ping }, { 8, 1, &pong } } };

    DenoisePC pc{
        .sizeX   = static_cast<int32_t>(WIDTH),
        .sizeY   = static_cast<int32_t>(HEIGHT),
        .marquee = marqueeRect(),   // re-composited, since the final pass overwrites the ray-gen overlay
    };
    for (std::size_t i = 0; i < passes.size(); ++i) {
        const Pass& p = passes[i];
        pc.step      = p.step;
        pc.firstPass = (i == 0) ? 1 : 0;
        pc.finalPass = (i + 1 == passes.size()) ? 1 : 0;

        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, denoisePipeLayout,
                                0, 1, &denoiseSets[p.set], 0, nullptr);
        vkCmdPushConstants(c, denoisePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(c, (WIDTH + 7) / 8, (HEIGHT + 7) / 8, 1);

        if (!pc.finalPass)   // this pass's output is the next pass's input
            imageBarrier(c, p.out->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
}

void SceneEditor::recordBlitToSwapchain(VkCommandBuffer c, uint32_t imageIndex) {
    const VkImage target = swapImages[imageIndex];

    // The source stage must be the stage the acquire semaphore is waited on
    // (TRANSFER): that forms the dependency chain which keeps this layout
    // transition from running before the presentation engine releases the
    // image. TOP_OF_PIPE here would race the acquire.
    imageBarrier(c, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Each view fills its half of the swapchain. If the surface size differs
    // from the window's logical size (DPI scaling), the blit scales.
    const auto halfW  = static_cast<int32_t>(swapExtent.width / VIEWS);
    const auto height = static_cast<int32_t>(swapExtent.height);
    const bool exact  = halfW == static_cast<int32_t>(WIDTH) && height == static_cast<int32_t>(HEIGHT);

    for (std::size_t vi = 0; vi < VIEWS; ++vi) {
        const RendererView& v = views[vi];
        const bool denoised = denoiseEnabled && vi == VIEW_PT;

        // Written by the final denoise pass (path-traced view) or by ray-gen.
        imageBarrier(c, v.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     denoised ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);

        // A blit (not a copy) converts RGBA -> BGRA swapchain formats.
        const auto x0 = static_cast<int32_t>(vi) * halfW;
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1]  = { static_cast<int32_t>(WIDTH), static_cast<int32_t>(HEIGHT), 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0]  = { x0, 0, 0 };
        blit.dstOffsets[1]  = { x0 + halfW, height, 1 };
        vkCmdBlitImage(c, v.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       exact ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }

    imageBarrier(c, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

// =============================================================================
//  Teardown (idempotent; also runs when initialization threw halfway)
// =============================================================================
void SceneEditor::cleanup() noexcept {
    if (dev) {
        vkDeviceWaitIdle(dev);

        for (RendererView& v : views) {
            destroyBuffer(v.sbt);
            if (v.pipeline) vkDestroyPipeline(dev, v.pipeline, nullptr);
            destroyImage(v.output);
            destroyImage(v.accum);
            destroyImage(v.history);
            v = {};
        }
        if (denoisePipeline)   vkDestroyPipeline(dev, denoisePipeline, nullptr);
        if (denoisePipeLayout) vkDestroyPipelineLayout(dev, denoisePipeLayout, nullptr);
        if (denoiseLayout)     vkDestroyDescriptorSetLayout(dev, denoiseLayout, nullptr);
        if (pipelineLayout)    vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
        if (descPool)          vkDestroyDescriptorPool(dev, descPool, nullptr);   // frees all sets
        if (descLayout)        vkDestroyDescriptorSetLayout(dev, descLayout, nullptr);
        for (Image* img : { &guide, &albedo, &ping, &pong }) destroyImage(*img);

        if (tlas) pvkDestroyAccelerationStructure(dev, tlas, nullptr);
        destroyBuffer(tlasBuffer);
        for (MeshInfo& m : meshes) {
            if (m.blas) pvkDestroyAccelerationStructure(dev, m.blas, nullptr);
            destroyBuffer(m.blasBuf);
        }
        for (Buffer* b : { &asScratch, &instanceBuffer, &objDataBuffer, &ubo, &vertexBuffer, &indexBuffer })
            destroyBuffer(*b);

        if (inFlight)          vkDestroyFence(dev, inFlight, nullptr);
        if (semImageAvailable) vkDestroySemaphore(dev, semImageAvailable, nullptr);
        if (cmdPool)           vkDestroyCommandPool(dev, cmdPool, nullptr);
        destroySwapchain();

        vkDestroyDevice(dev, nullptr);
        dev = VK_NULL_HANDLE;
    }

    if (instance) {
        if (debugMessenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance, debugMessenger, nullptr);
        }
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }

    if (hwnd) {
        // Detach first: DestroyWindow sends messages while this object is being destroyed.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (viewHwnd) SetWindowLongPtrW(viewHwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);   // also destroys the child viewport and toolbar buttons
        hwnd = viewHwnd = nullptr;
    }
}
