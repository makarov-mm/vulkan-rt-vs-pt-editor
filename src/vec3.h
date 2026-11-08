#pragma once
#include <cmath>

#include "cxx26.h"

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) noexcept : x(x_), y(y_), z(z_) {}

    [[nodiscard]] constexpr float dot(const Vec3& v) const noexcept { return x * v.x + y * v.y + z * v.z; }

    [[nodiscard]] constexpr Vec3 cross(const Vec3& v) const noexcept {
        return { y * v.z - z * v.y, z * v.x - x * v.z, x * v.y - y * v.x };
    }

    // constexpr once the standard library ships constexpr std::sqrt (C++26, P1383).
    [[nodiscard]] CXX26_CONSTEXPR_MATH float length() const noexcept { return std::sqrt(dot(*this)); }

    [[nodiscard]] CXX26_CONSTEXPR_MATH Vec3 normalize() const noexcept {
        const float l = length();
        return l > 0.0f ? Vec3{ x / l, y / l, z / l } : *this;
    }

    constexpr Vec3& operator+=(const Vec3& v) noexcept { x += v.x; y += v.y; z += v.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& v) noexcept { x -= v.x; y -= v.y; z -= v.z; return *this; }
    constexpr Vec3& operator*=(float s)       noexcept { x *= s;   y *= s;   z *= s;   return *this; }

    friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, const Vec3& b) noexcept { return a += b; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, const Vec3& b) noexcept { return a -= b; }
[[nodiscard]] constexpr Vec3 operator-(const Vec3& a)          noexcept { return { -a.x, -a.y, -a.z }; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 a, float s)        noexcept { return a *= s; }
[[nodiscard]] constexpr Vec3 operator*(float s, Vec3 a)        noexcept { return a *= s; }
[[nodiscard]] constexpr Vec3 operator/(Vec3 a, float s)        noexcept { return a *= 1.0f / s; }
