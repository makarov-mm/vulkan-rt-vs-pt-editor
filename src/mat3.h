#pragma once
#include <cmath>

#include "vec3.h"

struct Mat3
{
	Vec3 c0{ 1,0,0 }, c1{ 0,1,0 }, c2{ 0,0,1 };
};

constexpr Vec3 operator*(const Mat3& m, const Vec3& v) noexcept
{
    return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z;
}

constexpr Mat3 operator*(const Mat3& a, const Mat3& b) noexcept
{
    Mat3 r;
    r.c0 = a * b.c0;
    r.c1 = a * b.c1;
    r.c2 = a * b.c2;
    return r;
}

Mat3 rot_axis(const Vec3& a, float ang) noexcept;
