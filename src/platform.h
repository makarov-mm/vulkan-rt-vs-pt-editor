#pragma once

// Canonical include order: windows.h (lean, no min/max macros), then GDI+
// (which expects min/max, so std::min/max are aliased into its namespace),
// then Vulkan with the Win32 platform enabled. Include this header instead of
// any of them directly.

#ifndef UNICODE
#  define UNICODE   // the code calls the W APIs explicitly; this keeps macros like IDC_ARROW wide too
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>
#include <gdiplus.h>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#  define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
