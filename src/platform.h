#pragma once

// Win32 side of the editor. Canonical include order: windows.h (lean, no
// min/max macros), then GDI+ (which expects min/max, so std::min/max are
// aliased into its namespace), then the Vulkan Win32 surface API. Include this
// header first in every Win32 translation unit instead of any of them directly.

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

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>   // explicit, so it does not matter who included vulkan.h first
