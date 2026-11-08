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
#include "platform.h"

#include <cstdio>
#include <exception>
#include <print>
#include <string>
#include <string_view>

#include "config.h"
#include "scene_editor.h"

namespace {
// GDI+ for the PNG toolbar icons (a system library, not an external dependency).
// Declared before the editor, so the editor's bitmaps die before shutdown.
class GdiplusSession {
public:
    GdiplusSession() {
        const Gdiplus::GdiplusStartupInput input;
        started_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;   // else: GDI fallback icons
    }
    ~GdiplusSession() { if (started_) Gdiplus::GdiplusShutdown(token_); }
    GdiplusSession(const GdiplusSession&)            = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;

private:
    ULONG_PTR token_   = 0;
    bool      started_ = false;
};

// Debug builds open a console for the validation output; release builds only
// attach to the console they were started from, if any.
void attachConsole() {
    BOOL attached = FALSE;
    if constexpr (ENABLE_VALIDATION) attached = AllocConsole();
    else                             attached = AttachConsole(ATTACH_PARENT_PROCESS);
    if (!attached) return;
    SetConsoleOutputCP(CP_UTF8);
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}

// Exception messages are UTF-8 (/utf-8, file paths): MessageBoxW needs UTF-16.
std::wstring widen(std::string_view utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
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
        MessageBoxW(nullptr, widen(e.what()).c_str(), L"Vulkan RT Scene Editor - Error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
