// -----------------------------------------------------------------------------
//  Vulkan Hardware Ray Tracing — Scene Editor  (VK_KHR_ray_tracing_pipeline)
//
//  - Pure Win32 window + native icon toolbar, no GLFW / GLM / ImGui.
//  - One BLAS per shape type, built once. Every scene object is a TLAS
//    instance with its own transform, colour and material, so add / delete /
//    move / rotate only rewrite the instance buffer and rebuild the TLAS.
//  - Per-instance shading data lives in an SSBO indexed with
//    gl_InstanceCustomIndexEXT inside the closest-hit shader.
//  - Toolbar: shape buttons (click to add), select / move / rotate tools,
//    delete selected / delete all. LMB operates on objects, RMB orbits the
//    camera. Shift-click or a marquee (rubber band) selects multiple objects;
//    the selection gets a rim highlight rendered by the ray tracer itself.
// -----------------------------------------------------------------------------

#include <cstdio>
#include <exception>

#include "platform.h"
#include "scene_editor.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) 
{
    // Allocate a console so printf / validation output is visible.
    if (AllocConsole()) 
    {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    // GDI+ for the PNG toolbar icons (system library, no external dependency).
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    int rc = 0;

    try 
    {
        SceneEditor app;
        app.run(hInst);
    } 
	catch (const std::exception& e) 
    {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        MessageBoxA(nullptr, e.what(), "Vulkan RT Scene Editor - Error", MB_OK | MB_ICONERROR);
        rc = 1;
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    return rc;
}
