#include "mesh_builder.h"

#include <algorithm>
#include <array>
#include <numbers>
#include <span>

namespace {
constexpr float PI     = std::numbers::pi_v<float>;
constexpr float TWO_PI = 2.0f * PI;

constexpr int wrap(int i, int n) noexcept { return (i % n + n) % n; }

Vec3 position(const Vertex& v) noexcept { return { v.pos[0], v.pos[1], v.pos[2] }; }

// Superformula radius (Gielis), clamped so sharp presets cannot explode.
float superformula(float ang, float m, float n1, float n2, float n3) noexcept {
    const float t = m * ang * 0.25f;
    const float b = std::max(std::pow(std::fabs(std::cos(t)), n2) +
                             std::pow(std::fabs(std::sin(t)), n3), 1e-9f);
    return std::min(std::pow(b, -1.0f / n1), 6.0f);
}
} // namespace

// ---- bookkeeping ------------------------------------------------------------

void MeshBuilder::begin() noexcept {
    startVert_  = static_cast<uint32_t>(verts_.size());
    startIndex_ = static_cast<uint32_t>(indices_.size());
}

MeshRange MeshBuilder::end() const noexcept {
    MeshRange r;
    r.firstVert  = startVert_;
    r.vertCount  = static_cast<uint32_t>(verts_.size()) - startVert_;
    r.firstIndex = startIndex_;
    r.indexCount = static_cast<uint32_t>(indices_.size()) - startIndex_;

    // Bounding sphere: bbox centre + max distance; plus the lowest point.
    const std::span<const Vertex> vs(verts_.data() + r.firstVert, r.vertCount);
    Vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
    for (const Vertex& v : vs) {
        const Vec3 p = position(v);
        lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
        hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
    }
    r.bcenter = (lo + hi) * 0.5f;
    r.minY    = lo.y;

    float r2 = 0.0f;
    for (const Vertex& v : vs) {
        const Vec3 d = position(v) - r.bcenter;
        r2 = std::max(r2, d.dot(d));
    }
    r.bradius = std::sqrt(r2);
    return r;
}

uint32_t MeshBuilder::vertex(const Vec3& p, const Vec3& n) {
    verts_.push_back({ .pos = { p.x, p.y, p.z, 0.0f }, .nrm = { n.x, n.y, n.z, 0.0f } });
    return static_cast<uint32_t>(verts_.size() - 1);
}

void MeshBuilder::triangle(uint32_t a, uint32_t b, uint32_t c) {
    indices_.insert(indices_.end(), { a, b, c });
}

void MeshBuilder::quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    indices_.insert(indices_.end(), { a, b, c, a, c, d });
}

void MeshBuilder::flatTriangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& interior) {
    Vec3 n = (b - a).cross(c - a).normalize();
    const Vec3 centroid = (a + b + c) / 3.0f;
    if (n.dot(centroid - interior) < 0.0f) n = -n;
    const uint32_t i = vertex(a, n);
    vertex(b, n);
    vertex(c, n);
    triangle(i, i + 1, i + 2);
}

template <class PosFn>
void MeshBuilder::grid(int segU, int segV, bool outward, PosFn pos) {
    const auto base   = static_cast<uint32_t>(verts_.size());
    const auto stride = static_cast<uint32_t>(segV + 1);

    for (int i = 0; i <= segU; ++i) {
        for (int j = 0; j <= segV; ++j) {
            const Vec3 p  = pos(i, j);
            const Vec3 du = pos(i + 1, j) - pos(i - 1, j);
            const Vec3 dv = pos(i, j + 1) - pos(i, j - 1);
            Vec3 n = du.cross(dv);
            const float len = n.length();
            n = (len > 1e-5f) ? n / len : p.normalize();   // radial fallback at degenerate poles
            if (outward && n.dot(p) < 0.0f) n = -n;
            vertex(p, n);
        }
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(segU); ++i) {
        for (uint32_t j = 0; j < static_cast<uint32_t>(segV); ++j) {
            const uint32_t a = base + i * stride + j;
            const uint32_t b = a + stride;
            triangle(a, b, a + 1);
            triangle(a + 1, b, b + 1);
        }
    }
}

// ---- shapes -----------------------------------------------------------------

void MeshBuilder::sphere(float radius, int stacks, int slices) {
    const auto base = static_cast<uint32_t>(verts_.size());
    for (int i = 0; i <= stacks; ++i) {
        const float phi = PI * static_cast<float>(i) / static_cast<float>(stacks);
        for (int j = 0; j <= slices; ++j) {
            const float theta = TWO_PI * static_cast<float>(j) / static_cast<float>(slices);
            const Vec3 n{ std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta) };
            vertex(n * radius, n);
        }
    }
    const auto ring = static_cast<uint32_t>(slices + 1);
    for (uint32_t i = 0; i < static_cast<uint32_t>(stacks); ++i) {
        for (uint32_t j = 0; j < static_cast<uint32_t>(slices); ++j) {
            const uint32_t a = base + i * ring + j;
            const uint32_t b = a + ring;
            triangle(a, b, a + 1);
            triangle(a + 1, b, b + 1);
        }
    }
}

void MeshBuilder::diamond(float s, int n) {
    // Proportions: table radius / height, girdle radius, pavilion depth.
    constexpr float tableR = 0.50f, tableH = 0.42f, girdleR = 1.00f, pavilionH = 1.15f;

    const Vec3 interior{ 0.0f, (tableH - pavilionH) * 0.5f * s, 0.0f };
    const Vec3 tableCenter{ 0.0f, tableH * s, 0.0f };
    const Vec3 culet{ 0.0f, -pavilionH * s, 0.0f };

    auto table = [&](int i) {
        const float a = TWO_PI * static_cast<float>(wrap(i, n)) / static_cast<float>(n);
        return Vec3{ tableR * std::cos(a) * s, tableH * s, tableR * std::sin(a) * s };
    };
    auto girdle = [&](int i) {
        const float a = TWO_PI * (static_cast<float>(wrap(i, n)) + 0.5f) / static_cast<float>(n);
        return Vec3{ girdleR * std::cos(a) * s, 0.0f, girdleR * std::sin(a) * s };
    };

    for (int i = 0; i < n; ++i) {
        flatTriangle(tableCenter, table(i + 1), table(i), interior);   // table
        flatTriangle(table(i), table(i + 1), girdle(i), interior);     // crown
        flatTriangle(table(i + 1), girdle(i + 1), girdle(i), interior);
        flatTriangle(girdle(i), girdle(i + 1), culet, interior);       // pavilion
    }
}

void MeshBuilder::cube(float size) {
    constexpr std::array<Vec3, 6> normals{ {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } } };
    const float h = size * 0.5f;

    for (const Vec3& n : normals) {
        const bool vertical = std::fabs(n.y) > 0.5f;
        const Vec3 u = vertical ? Vec3{ 1, 0, 0 } : Vec3{ 0, 1, 0 };
        const Vec3 w = vertical ? Vec3{ 0, 0, 1 } : n.cross(u);
        const Vec3 c = n * h;
        const uint32_t i = vertex(c - u * h - w * h, n);
        vertex(c + u * h - w * h, n);
        vertex(c + u * h + w * h, n);
        vertex(c - u * h + w * h, n);
        quad(i, i + 1, i + 2, i + 3);
    }
}

void MeshBuilder::pyramid(float base, float height) {
    const float hs = base * 0.5f;
    const Vec3 interior{ 0.0f, height * 0.30f, 0.0f };
    const Vec3 b0{ -hs, 0, -hs }, b1{ hs, 0, -hs }, b2{ hs, 0, hs }, b3{ -hs, 0, hs };
    const Vec3 apex{ 0, height, 0 };

    flatTriangle(b0, b1, apex, interior);
    flatTriangle(b1, b2, apex, interior);
    flatTriangle(b2, b3, apex, interior);
    flatTriangle(b3, b0, apex, interior);
    flatTriangle(b0, b2, b1, interior);   // base
    flatTriangle(b0, b3, b2, interior);
}

void MeshBuilder::cylinder(float r, float h, int segments) {
    const Vec3 base{ 0, 0, 0 }, top{ 0, h, 0 };
    const Vec3 down{ 0, -1, 0 }, up{ 0, 1, 0 };

    for (int i = 0; i < segments; ++i) {
        const float a0 = TWO_PI * static_cast<float>(i)     / static_cast<float>(segments);
        const float a1 = TWO_PI * static_cast<float>(i + 1) / static_cast<float>(segments);
        const Vec3 d0{ std::cos(a0), 0, std::sin(a0) }, d1{ std::cos(a1), 0, std::sin(a1) };

        // Side (smooth radial normals).
        const uint32_t s = vertex(base + d0 * r, d0);
        vertex(base + d1 * r, d1);
        vertex(top + d1 * r, d1);
        vertex(top + d0 * r, d0);
        quad(s, s + 1, s + 2, s + 3);

        // Flat caps.
        const uint32_t cb = vertex(base, down);
        vertex(base + d1 * r, down);
        vertex(base + d0 * r, down);
        triangle(cb, cb + 1, cb + 2);

        const uint32_t ct = vertex(top, up);
        vertex(top + d0 * r, up);
        vertex(top + d1 * r, up);
        triangle(ct, ct + 1, ct + 2);
    }
}

void MeshBuilder::dodecahedron(float circumradius) {
    constexpr float P = std::numbers::phi_v<float>, a = 1.0f / P;
    constexpr std::array<Vec3, 20> verts{ {
        { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, -1 }, { -1, 1, 1 },
        {  1, -1, -1 }, {  1, -1, 1 }, {  1, 1, -1 }, {  1, 1, 1 },
        { 0, -a, -P }, { 0, -a, P }, { 0, a, -P }, { 0, a, P },
        { -a, -P, 0 }, { -a, P, 0 }, { a, -P, 0 }, { a, P, 0 },
        { -P, 0, -a }, { -P, 0, a }, { P, 0, -a }, { P, 0, a } } };
    constexpr int faces[12][5] = {
        { 14, 12, 0, 8, 4 }, { 10, 8, 0, 16, 2 }, { 17, 16, 0, 12, 1 }, { 5, 9, 1, 12, 14 },
        { 3, 17, 1, 9, 11 }, { 6, 10, 2, 13, 15 }, { 3, 13, 2, 16, 17 }, { 15, 13, 3, 11, 7 },
        { 6, 18, 4, 8, 10 }, { 5, 14, 4, 18, 19 }, { 11, 9, 5, 19, 7 }, { 19, 18, 6, 15, 7 } };

    const float scale = circumradius / std::numbers::sqrt3_v<float>;   // circumradius of `verts` is sqrt(3)
    const Vec3 center{ 0, 0, 0 };
    for (const auto& face : faces) {
        std::array<Vec3, 5> v;
        for (int k = 0; k < 5; ++k) v[k] = verts[face[k]] * scale;
        flatTriangle(v[0], v[1], v[2], center);   // pentagon as a fan
        flatTriangle(v[0], v[2], v[3], center);
        flatTriangle(v[0], v[3], v[4], center);
    }
}

void MeshBuilder::supertoroid(int segU, int segV) {
    constexpr float exponent = 2.2f, twist = 3.0f, majorR = 2.2f;
    grid(segU, segV, false, [&](int i, int j) {
        const float u  = TWO_PI * static_cast<float>(wrap(i, segU)) / static_cast<float>(segU);
        const float v  = TWO_PI * static_cast<float>(wrap(j, segV)) / static_cast<float>(segV);
        const float cv = std::max(std::fabs(std::cos(v)), 1e-9f);
        const float sv = std::max(std::fabs(std::sin(v)), 1e-9f);
        const float rr = std::pow(std::pow(cv, exponent) + std::pow(sv, exponent), -1.0f / exponent);
        const float phi = twist * u + v;
        const float r   = majorR + rr * std::cos(phi);
        return Vec3{ r * std::cos(u), rr * std::sin(phi), r * std::sin(u) };
    });
}

void MeshBuilder::supershape(int segU, int segV) {
    constexpr float m = 6, n1 = 5, n2 = 5, n3 = 5;
    auto raw = [&](int i, int j) {
        const float th = -PI + TWO_PI * static_cast<float>(wrap(i, segU)) / static_cast<float>(segU);
        const float ph = -0.5f * PI + PI * static_cast<float>(wrap(j, segV)) / static_cast<float>(segV);
        const float r1 = superformula(th, m, n1, n2, n3);
        const float r2 = superformula(ph, m, n1, n2, n3);
        return Vec3{ r1 * std::cos(th) * r2 * std::cos(ph),
                     r2 * std::sin(ph),
                     r1 * std::sin(th) * r2 * std::cos(ph) };
    };

    // Normalise to a fixed size.
    float maxR = 1e-6f;
    for (int i = 0; i <= segU; ++i)
        for (int j = 0; j <= segV; ++j) maxR = std::max(maxR, raw(i, j).length());

    const float scale = 1.6f / maxR;
    grid(segU, segV, true, [&](int i, int j) { return raw(i, j) * scale; });
}

void MeshBuilder::floor(float half) {
    const Vec3 n{ 0, 1, 0 };
    const uint32_t i = vertex({ -half, 0, -half }, n);
    vertex({ half, 0, -half }, n);
    vertex({ half, 0, half }, n);
    vertex({ -half, 0, half }, n);
    quad(i, i + 1, i + 2, i + 3);
}
