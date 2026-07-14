#pragma once
#include <cmath>

#include "cxx26.h"

struct Vec3 
{
    float x = 0, y = 0, z = 0;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) { }

    constexpr float dot(const Vec3& v) const noexcept
    {
        return x * v.x + y * v.y + z * v.z;
    }

    constexpr Vec3 cross(const Vec3& v) const noexcept
    {
        return {
            y * v.z - z * v.y,
            z * v.x - x * v.z,
            x * v.y - y * v.x
        };
    }

    // constexpr once the standard library ships constexpr std::sqrt
    // (C++26, P1383); see CXX26_CONSTEXPR_MATH in cxx26.h.
    CXX26_CONSTEXPR_MATH Vec3 normalize() const noexcept
    {
        float l = std::sqrt(dot(*this));
        return l > 0 ? Vec3{ x / l, y / l, z / l } : *this;
    }
};

constexpr Vec3 operator+(const Vec3& a, const Vec3& b) noexcept
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

constexpr Vec3 operator-(const Vec3& a, const Vec3& b) noexcept
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

constexpr Vec3 operator*(const Vec3& a, float s) noexcept
{
	return { a.x * s, a.y * s, a.z * s };
}
