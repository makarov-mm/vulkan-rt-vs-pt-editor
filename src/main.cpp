// -----------------------------------------------------------------------------
//  Vulkan Hardware Ray Tracing vs Path Tracing -- Scene Editor
//  (VK_KHR_ray_tracing_pipeline)
//
//  - Pure Win32 window + native icon toolbar; no GLFW / GLM / ImGui.
//  - One BLAS per shape type, built once. Every scene object is a TLAS
//    instance with its own transform, colour and material, so add / delete /
//    move / rotate only rewrite the instance buffer and rebuild the TLAS.
//  - Left half: Whitted ray tracing. Right half: Monte Carlo path tracing with
//    an SVGF-style a-trous denoiser. Same scene, camera and TLAS.
// -----------------------------------------------------------------------------
#include <cstdio>
#include <exception>
#include <print>

#include "platform.h"
#include "scene_editor.h"

namespace {
// GDI+ for the PNG toolbar icons (a system library, not an external dependency).
// Declared before the editor, so the editor's bitmaps die before shutdown.
class GdiplusSession {
public:
    GdiplusSession() {
        const Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token_, &input, nullptr);
    }
    ~GdiplusSession() { Gdiplus::GdiplusShutdown(token_); }
    GdiplusSession(const GdiplusSession&)            = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;

private:
    ULONG_PTR token_ = 0;
};

// A console window so validation messages and stdout are visible.
void attachConsole() {
    if (!AllocConsole()) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}
} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    attachConsole();
    GdiplusSession gdiplus;

    try {
        SceneEditor editor;
        editor.run(hInst);
        return 0;
    } catch (const std::exception& e) {
        std::println(stderr, "FATAL: {}", e.what());
        MessageBoxA(nullptr, e.what(), "Vulkan RT Scene Editor - Error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
