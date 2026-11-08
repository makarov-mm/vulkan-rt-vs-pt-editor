#pragma once
#include "platform.h"   // first: windows.h, GDI+, Vulkan Win32

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "camera.h"
#include "config.h"
#include "cxx26.h"
#include "editor_ui.h"
#include "gpu_types.h"
#include "presenter.h"
#include "renderer.h"
#include "scene.h"
#include "vk_context.h"

// =============================================================================
//  Scene editor: Win32 window + toolbar, editor state and the frame loop that
//  drives the two ray tracing renderers.
//
//  Implementation is split by concern:
//    scene_editor.cpp    lifecycle, frame loop, editor actions
//    editor_window.cpp   window procedures, toolbar, icons
//    editor_input.cpp    mouse / keyboard, picking, marquee selection
//  The Vulkan side (vk_context, renderer, presenter) knows nothing about Win32.
// =============================================================================
class SceneEditor {
public:
    SceneEditor() = default;
    ~SceneEditor();

    // The editor owns raw Win32 handles and the Vulkan objects; a copy would destroy them twice.
    SceneEditor(const SceneEditor&)            = CXX26_DELETED("SceneEditor owns the window and the Vulkan objects; copying would destroy them twice");
    SceneEditor& operator=(const SceneEditor&) = CXX26_DELETED("SceneEditor owns the window and the Vulkan objects; copying would destroy them twice");

    void run(HINSTANCE hInst);

private:
    // ---- Win32 --------------------------------------------------------------
    HWND hwnd_     = nullptr;   // main window (hosts the toolbar)
    HWND viewHwnd_ = nullptr;   // child window the swapchain presents into
    std::array<HWND, 3> toolButtons_{};   // select / move / rotate, repainted on mode change
    bool quit_ = false;
    std::exception_ptr pendingError_;     // thrown inside a window procedure, rethrown by the main loop

    static constexpr int ICON_COUNT = ADDABLE_MESHES + 5;   // shapes + 3 tools + 2 delete buttons
    std::array<std::unique_ptr<Gdiplus::Bitmap>, ICON_COUNT> icons_;   // nullptr -> GDI fallback

    // ---- Camera / accumulation ----------------------------------------------
    OrbitCamera camera_;
    bool  orbiting_   = false;
    POINT lastMouse_{};
    bool  resetAccum_ = true;   // restart temporal accumulation next frame

    // ---- Editor tool state --------------------------------------------------
    EditMode mode_        = EditMode::Select;
    bool     lmbDown_     = false;
    bool     dragMoved_   = false;
    POINT    downMouse_{};
    int      viewOffsetX_ = 0;     // x of the half the current interaction started in

    bool  marqueeOn_ = false;      // rubber band, in view-local pixels
    POINT mq0_{}, mq1_{};

    bool  movingObjs_ = false;     // move drag: ground plane, vertical with Shift
    float grabPlaneY_ = 0.0f;
    Vec3  grabPoint_;
    Vec3  dragHoriz_;
    float dragVert_   = 0.0f;
    std::vector<std::pair<std::size_t, Vec3>> dragOrig_;   // object index -> original position

    bool rotatingObjs_ = false;

    // ---- Scene --------------------------------------------------------------
    MeshTable                meshes_;               // prototype bounds: picking and on-floor placement
    std::vector<SceneObject> objects_;              // editable objects; instance slot = FIXED_SLOTS + index
    bool  sceneDirty_     = true;                   // instances / object data / TLAS need a rebuild
    float spawnAngle_     = 0.7f;                   // golden-angle walk for new object placement
    bool  denoiseEnabled_ = true;                   // 'D' toggles raw vs denoised path tracing

    // ---- Vulkan (created in this order, destroyed in reverse) ---------------
    std::optional<VulkanContext> vk_;
    std::optional<Renderer>      renderer_;
    std::optional<Presenter>     presenter_;

    // =========================================================================
    //  scene_editor.cpp
    // =========================================================================
    void initVulkan();
    void mainLoop();
    void drawFrame();

    // Directory `name` (shaders, icons) next to the executable -- where the
    // post-build step copies it -- or else relative to the working directory.
    [[nodiscard]] static std::filesystem::path assetDir(const wchar_t* name);

    // Editor actions
    void markSceneDirty() noexcept;
    void clearSelection() noexcept;
    void addObject(MeshType type);
    void deleteSelected();
    void deleteAll();
    void endObjectDrag() noexcept;   // dragOrig_ holds indices: any change to the object list ends the drag

    // =========================================================================
    //  editor_window.cpp
    // =========================================================================
    static LRESULT CALLBACK mainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK viewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    [[nodiscard]] std::optional<LRESULT> onMainMessage(UINT msg, WPARAM wp, LPARAM lp);   // nullopt: default handling
    [[nodiscard]] std::optional<LRESULT> onViewMessage(UINT msg, WPARAM wp, LPARAM lp);

    void createWindow(HINSTANCE hInst);
    void destroyWindow() noexcept;
    void createToolbar(HINSTANCE hInst);
    void loadIcons();

    void onCommand(int id);
    BOOL onDrawItem(const DRAWITEMSTRUCT& d);
    void drawFallbackIcon(HDC dc, const RECT& rc, int id) const;
    [[nodiscard]] static int iconIndex(int id) noexcept;

    // =========================================================================
    //  editor_input.cpp
    // =========================================================================
    void onKeyDown(WPARAM key, LPARAM flags);
    void onWheel(float steps);
    void onRButtonDown(int x, int y);
    void onRButtonUp();
    void onLButtonDown(int x, int y);
    void onMouseMove(int x, int y);
    void onLButtonUp(int x, int y);
    void onCaptureLost() noexcept;

    // Maps a viewport x into the per-view (WIDTH wide) coordinates of the half
    // the current interaction started in, so a drag cannot jump at the seam.
    [[nodiscard]] int localX(int x) const noexcept;
    [[nodiscard]] static bool shiftHeld() noexcept { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }

    // Picking and marquee selection
    [[nodiscard]] Vec3 worldCenter(const SceneObject& o) const noexcept;
    [[nodiscard]] std::optional<std::size_t> pickObject(int px, int py) const noexcept;
    void marqueeSelect(bool additive);
    [[nodiscard]] Float4 marqueeRect() const noexcept;
};
