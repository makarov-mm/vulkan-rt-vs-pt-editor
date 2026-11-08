// Lifecycle, frame loop and editor actions.
#include "scene_editor.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <system_error>

namespace {
constexpr std::array<Vec3, 8> SPAWN_PALETTE{ {
    { 0.55f, 0.72f, 1.00f }, { 1.00f, 0.65f, 0.72f }, { 0.55f, 0.95f, 0.65f },
    { 1.00f, 0.85f, 0.45f }, { 0.45f, 0.92f, 0.92f }, { 0.78f, 0.60f, 1.00f },
    { 1.00f, 0.55f, 0.35f }, { 0.90f, 0.90f, 0.98f },
} };

std::filesystem::path executableDir() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) { path.resize(n); break; }
        path.resize(path.size() * 2);   // truncated: long path
    }
    return std::filesystem::path(path).parent_path();
}
} // namespace

SceneEditor::~SceneEditor() {
    // Vulkan first (the surface refers to the viewport window), then the window.
    presenter_.reset();
    renderer_.reset();
    vk_.reset();
    destroyWindow();
}

void SceneEditor::run(HINSTANCE hInst) {
    createWindow(hInst);
    initVulkan();
    mainLoop();
}

std::filesystem::path SceneEditor::assetDir(const wchar_t* name) {
    std::error_code ec;
    if (auto dir = executableDir() / name; std::filesystem::is_directory(dir, ec)) return dir;
    return name;
}

void SceneEditor::initVulkan() {
    constexpr std::array<const char*, 2> surfaceExtensions{ VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    vk_.emplace(surfaceExtensions, [this](VkInstance instance) {
        const VkWin32SurfaceCreateInfoKHR ci{
            .sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = GetModuleHandleW(nullptr),
            .hwnd      = viewHwnd_,   // the swapchain lives in the viewport child window
        };
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        vkCheck(vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface), "vkCreateWin32SurfaceKHR");
        return surface;
    });

    const PrototypeMeshes prototypes = buildPrototypeMeshes();
    meshes_  = prototypes.meshes;
    objects_ = defaultScene(meshes_);

    renderer_.emplace(*vk_, prototypes, assetDir(L"shaders"));
    presenter_.emplace(*vk_, VkExtent2D{ WIDTH * VIEWS, HEIGHT });
}

void SceneEditor::mainLoop() {
    MSG msg{};
    while (!quit_) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit_ = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (pendingError_) std::rethrow_exception(std::exchange(pendingError_, nullptr));
        if (quit_) break;

        // Nothing is visible while minimized: sleep until a message arrives
        // instead of spinning on a zero-sized swapchain.
        if (IsIconic(hwnd_)) { WaitMessage(); continue; }

        drawFrame();
    }
}

void SceneEditor::drawFrame() {
    const auto frame = presenter_->beginFrame();
    if (!frame) return;

    // The previous frame has finished (beginFrame waited for it), so the
    // renderer can rewrite its host-visible buffers without racing the GPU.
    renderer_->render(frame->cmd, {
        .camera              = camera_,
        .objects             = objects_,
        .marquee             = marqueeRect(),
        .sceneChanged        = std::exchange(sceneDirty_, false),
        .restartAccumulation = std::exchange(resetAccum_, false),
        .denoise             = denoiseEnabled_,
    });

    const std::array<VkImage, VIEWS> outputs{ renderer_->output(Renderer::VIEW_RT), renderer_->output(Renderer::VIEW_PT) };
    presenter_->endFrame(*frame, outputs, Renderer::extent());
}

// =============================================================================
//  Editor actions
// =============================================================================
void SceneEditor::markSceneDirty() noexcept {
    sceneDirty_ = true;
    resetAccum_ = true;   // restart accumulation so edits show up clean
}

void SceneEditor::clearSelection() noexcept {
    for (SceneObject& o : objects_) o.selected = false;
}

void SceneEditor::endObjectDrag() noexcept {
    movingObjs_   = false;
    rotatingObjs_ = false;
    dragOrig_.clear();
}

void SceneEditor::addObject(MeshType type) {
    if (FIXED_SLOTS + objects_.size() >= MAX_INSTANCES) {
        const auto text = std::format(L"Instance limit reached ({}).", MAX_INSTANCES);
        MessageBoxW(hwnd_, text.c_str(), L"Scene Editor", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Walk a golden-angle spiral so new objects don't stack on each other.
    spawnAngle_ += 2.39996f;
    const float radius = 4.2f + 1.4f * static_cast<float>((objects_.size() / 6) % 3);

    clearSelection();
    objects_.push_back({
        .mesh     = type,
        .pos      = { std::cos(spawnAngle_) * radius,
                      -meshes_[type].minY,                  // base sits on the floor
                      std::sin(spawnAngle_) * radius },
        .color    = SPAWN_PALETTE[objects_.size() % SPAWN_PALETTE.size()],
        .material = Material::Glass,
        .selected = true,                                   // the new object becomes the selection
    });
    markSceneDirty();
}

void SceneEditor::deleteSelected() {
    if (std::erase_if(objects_, [](const SceneObject& o) { return o.selected; }) > 0) {
        endObjectDrag();   // Del pressed mid-drag: the stored indices are stale now
        markSceneDirty();
    }
}

void SceneEditor::deleteAll() {
    if (objects_.empty()) return;
    objects_.clear();
    endObjectDrag();
    markSceneDirty();
}
