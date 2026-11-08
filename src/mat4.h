#pragma once
#include <array>

#include "vec3.h"

// Column-major 4x4 matrix, laid out exactly like a GLSL mat4 (16 floats).
struct Mat4 {
    std::array<float, 16> m{};

    [[nodiscard]] static constexpr Mat4 identity() noexcept {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
};
static_assert(sizeof(Mat4) == 64, "Mat4 must match a GLSL mat4");
