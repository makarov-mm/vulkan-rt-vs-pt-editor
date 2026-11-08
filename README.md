# Vulkan Ray Tracing vs Path Tracing - Scene Editor

[![GitHub Sponsors](https://img.shields.io/github/sponsors/makarov-mm?style=flat&logo=github)](https://github.com/sponsors/makarov-mm)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![C++26](https://img.shields.io/badge/dialect-C%2B%2B26-blue)](https://en.cppreference.com/w/cpp/26)
![Vulkan](https://img.shields.io/badge/Vulkan-AC162C?logo=vulkan&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-0078D6?logo=windows&logoColor=white)
[![LinkedIn](https://img.shields.io/badge/LinkedIn-0A66C2?logo=linkedin&logoColor=white)](https://www.linkedin.com/in/makarov-mm/)
[![Threads](https://img.shields.io/badge/Threads-000000?logo=threads&logoColor=white)](https://www.threads.net/@m.m.makarov)
[![Instagram](https://img.shields.io/badge/Instagram-E4405F?logo=instagram&logoColor=white)](https://www.instagram.com/m.m.makarov/)

An interactive scene editor rendered by two hardware-accelerated renderers at
once (`VK_KHR_ray_tracing_pipeline`): the left half of the window is a classic
Whitted-style ray tracer, the right half is a Monte Carlo path tracer with
global illumination and an SVGF-style denoiser. Both halves share one scene,
one camera and one TLAS, so every edit, drag and orbit is perfectly
synchronized - a live side-by-side of what 1980 ray tracing misses and 1986
path tracing integrates: shadow fill, colour bleeding, caustics.

The two renderers share the miss, shadow-miss and closest-hit shaders, the
whole editor and all common ray-gen code (`shaders/raygen_common.glsl`); they
differ only in how surfaces are shaded - the ray-gen shader
(`raygen_rt.rgen` vs `raygen_pt.rgen`). The display transform (ACES + gamma),
sky, materials and light levels are calibrated to match, so any difference you
see on screen is algorithmic, not tonal.

Zero external dependencies beyond the Vulkan SDK itself: the window and the
toolbar are raw Win32, the icons load through GDI+ (part of Windows), and all
math is inline (no GLFW, no ImGui, no GLM).

## Screenshot
![Screenshot](screenshot.png)  

## Toolbar

| Group | Buttons |
|-------|---------|
| Shapes | Sphere, diamond, cube, pyramid, cylinder, dodecahedron, supertoroid, supershape - click to add one to the scene |
| Tools  | **Select** (cursor), **Move**, **Rotate** - the active tool is highlighted |
| Danger | **Delete selected** (trash), **Delete all** (trash + ×) |

Icons are pre-rendered anti-aliased PNGs in `icons/`, loaded at runtime with
GDI+ (a system library that ships with Windows, so the zero-dependency rule
still holds). If a PNG is missing, the button falls back to a GDI-drawn icon.

## Mouse

| Input | Action |
|-------|--------|
| **RMB drag** | Orbit the camera (any tool, both halves in sync) |
| Mouse wheel | Zoom |
| **LMB**, Select tool | Click an object to select it; click empty space to deselect; **Shift-click** toggles objects in and out of the selection; **drag** draws a marquee (rubber band) that selects everything it touches (Shift adds to the selection) |
| **LMB drag**, Move tool | Drag the selection in the ground plane; hold **Shift** to move vertically. Grabbing an unselected object selects it first |
| **LMB drag**, Rotate tool | Horizontal drag spins the selection about the world Y axis, vertical drag about the camera's right axis |
| Del | Delete the selection |
| D | Toggle the denoiser on / off (path-traced half) |
| Esc | Quit |

Clicks and drags land in whichever half the cursor is over - both halves show
the same scene from the same camera, so the editor maps the input into that
half's local coordinates (a drag stays anchored to the half it started in).
The selection marquee is drawn by both renderers at the same scene position.

Selected objects get a warm rim highlight rendered by the ray tracer itself;
the marquee rectangle is also composited by the ray-gen shader over the final
image, so there is no GDI flicker over the swapchain.

## Rendering features

- **Monte Carlo path tracing with global illumination** - diffuse surfaces
  bounce with cosine-weighted importance sampling, so indirect light and colour
  bleeding come out of the estimator instead of an ambient constant. Direct
  light uses next event estimation toward the spherical area light, combined
  with BSDF sampling via multiple importance sampling (power heuristic);
  Russian roulette terminates long paths without bias. The HDR accumulation is
  tone-mapped with ACES.
- **SVGF-style a-trous denoiser** - a compute pass after ray tracing runs four
  iterations of an edge-avoiding a-trous wavelet filter guided by the primary
  hit's normal and depth. Radiance is demodulated by the primary-hit albedo
  before filtering (the checkerboard and object colours stay pixel-sharp), the
  filter strength adapts to the per-pixel sample count (1/sqrt(n)), and glass
  pixels pass through unfiltered. Press `D` to compare against the raw
  path-traced output; the editor marquee is re-composited by the final pass.
- **Instanced acceleration structures** - one BLAS per shape type, built once
  with `PREFER_FAST_TRACE`. Every scene object is a TLAS instance carrying its
  own 3×4 transform, so moving, rotating, adding or deleting an object only
  rewrites the instance buffer and rebuilds the TLAS - the triangle geometry
  is never touched.
- **Per-instance materials** - colour, material id, reflectivity and the
  selection flag live in an SSBO indexed with `gl_InstanceCustomIndexEXT` in
  the closest-hit shader; normals go from object to world space through
  `gl_ObjectToWorldEXT`.
- **Dispersive Fresnel glass** - per-channel IOR 1.50/1.52/1.54 with
  hero-wavelength sampling, total internal reflection, coloured glass tint,
  and real refractive caustics on the floor as the accumulation converges.
- **Soft shadows** from a visible spherical area light, which is also the key
  light of the path-traced scene (cone sampling with a distance-dependent
  penumbra).
- **Per-pixel adaptive temporal accumulation** - static pixels converge to a
  clean image; pixels whose primary hit moved reset, so dragged objects stay
  sharp. Glass pixels additionally track a deterministic path-length signature
  through the refractive chain, so dragging an object behind or inside glass
  resets exactly the affected pixels instead of smearing into ghosts. Any edit
  also restarts accumulation so changes always show up clean.
- Ray-gen / closest-hit / two miss shaders, a correctly aligned shader binding
  table, `maxPipelineRayRecursionDepth = 1` (all bounces iterate in ray-gen).
- Vulkan 1.3 synchronization2: every barrier carries its own stages and
  accesses, and the barriers of one dependency point are recorded with a
  single `vkCmdPipelineBarrier2`.
- Vertex and index data live in device-local memory (staged upload); the small
  per-frame buffers (UBO, instances, object data) use device-local host-visible
  memory when the GPU exposes it (resizable BAR) and stay persistently mapped.

## C++

The project builds with `/std:c++latest` and uses C++23 throughout
(`std::print`, `std::format`, ranges, `std::to_underlying`, designated
initializers, `std::source_location` in Vulkan error messages). MSVC
implements only part of the C++26 paper set so far, so every C++26 feature is
adopted through its standard feature test macro in `src/cxx26.h` and degrades
gracefully until the toolchain catches up:

- `std::inplace_vector` (P0843) for Vulkan descriptor scratch arrays, where
  its by-type pointer-stability guarantee is exactly what
  `vkUpdateDescriptorSets` needs, and for barrier batches (minimal polyfill
  until MSVC STL ships it)
- `= delete("reason")` (P2573) on the copy operations of every owner of raw
  Vulkan / Win32 handles, documenting *why* it must not be copied
- constexpr `std::sqrt` (P1383) making `Vec3::normalize` constexpr the moment
  the standard library allows it
- GPU-side layout contracts (`FrameUniforms`, `GpuObj`, `Vertex`, the denoiser
  push constants) are pinned with `static_assert`, so a host/GLSL layout
  mismatch fails the build instead of rendering garbage

## Requirements

- Windows 10/11, x64
- A GPU with Vulkan 1.3 and `VK_KHR_ray_tracing_pipeline` support (NVIDIA RTX,
  AMD RX 6000+, Intel Arc) and recent drivers
- [Vulkan SDK](https://vulkan.lunarg.com/) (the `VULKAN_SDK` environment
  variable must be set - the installer does this)
- Visual Studio 2026 (toolset v145) with the C++ workload

## Building

Open `VulkanRTvsPTEditor.slnx` and build (x64, Debug or Release). The
pre-build step compiles the GLSL shaders to SPIR-V with the SDK's
`glslangValidator`; the post-build step copies them and the icons next to the
executable, where the editor looks for them first (then in the working
directory). Debug builds open a console and enable the Khronos validation
layer when it is installed; release builds only print to the console they
were started from.

## Source layout

The Vulkan side (`vk_context`, `renderer`, `presenter`) is platform
independent; everything Win32 lives in the editor files.

| File | Contents |
|------|----------|
| `src/main.cpp` | Entry point, GDI+ session, console, error reporting |
| `src/scene_editor.h` / `.cpp` | Editor class: lifecycle, frame loop, editor actions |
| `src/editor_window.cpp` | Win32 window procedures, toolbar, icons |
| `src/editor_input.cpp` | Mouse / keyboard, picking, marquee selection |
| `src/vk_context.h` / `.cpp` | Instance, device, queue, ray tracing entry points, buffer / image / shader helpers |
| `src/renderer.h` / `.cpp` | Both renderers: acceleration structures, images, pipelines, SBT, denoiser, per-frame recording |
| `src/presenter.h` / `.cpp` | Swapchain, frame pacing, blit and present |
| `src/vk_utils.h` / `.cpp` | `vkCheck`, buffer / image handles, synchronization2 barrier batches |
| `src/scene.h` / `.cpp` | Shape types, materials, scene objects; prototype meshes and the default scene |
| `src/mesh_builder.h` / `.cpp` | Procedural shape generation |
| `src/camera.h` / `.cpp` | Orbit camera; CPU rays and projection matching the shaders |
| `src/gpu_types.h` | Host mirrors of the shader data layouts |
| `src/config.h` | Window size, light, camera and quality constants |
| `shaders/payload.glsl` | Hit payload and material ids shared by all ray tracing stages |
| `shaders/raygen_common.glsl` | Everything both ray-gen shaders share |
| `shaders/display.glsl` | Tone map and marquee overlay (ray-gen and denoiser) |

## Architecture notes

The editor path is deliberately cheap. On any scene edit:

1. The instance array (`VkAccelerationStructureInstanceKHR`, one per object)
   and the per-object shading SSBO are rewritten on the host (both persistently
   mapped) once the previous frame has finished (one frame in flight).
2. The TLAS rebuild (`PREFER_FAST_BUILD`) is recorded at the start of that
   frame's command buffer, followed by a barrier to the ray tracing stage - no
   extra submit and no queue stall. With at most 64 instances it is
   sub-millisecond.
3. The accumulation counter resets, and the per-pixel history test
   additionally resets exactly the pixels whose primary hit moved.

BLASes are static and never rebuilt, which is what lets them use
`PREFER_FAST_TRACE` for maximum traversal performance. All of them are built
at startup in a single command, each with its own slice of one scratch buffer. Object picking is a
CPU ray against per-instance bounding spheres; marquee selection projects the
same bounding spheres to the screen and tests them against the rectangle.

## License

MIT License

Copyright (c) 2026 Mykhailo Makarov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Support

If you found this project interesting or useful, you can support my work:

[![GitHub Sponsors](https://img.shields.io/github/sponsors/makarov-mm?style=flat&logo=github)](https://github.com/sponsors/makarov-mm)
