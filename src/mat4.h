#pragma once
#include <cmath>

#include "vec3.h"

struct Mat4
{
	float m[16] = { };

    static constexpr Mat4 identity() noexcept
    {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    Mat4 inverse() const noexcept;
    void multiply(const float v[4], float out[4]) const noexcept;
};

Mat4 lookAtRH(Vec3 eye, Vec3 center, Vec3 up) noexcept;
Mat4 perspectiveVk(float fovY, float aspect, float zNear, float zFar) noexcept;
