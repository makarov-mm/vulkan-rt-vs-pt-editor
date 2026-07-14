#pragma once

// Canonical include order for this project: windows.h (lean, no min/max
// macros), then GDI+ (needs std::min/max aliased into its namespace because
// NOMINMAX removed the macros it expects), then Vulkan with the Win32
// platform enabled. Include this header first everywhere instead of
// repeating the dance.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
