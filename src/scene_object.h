#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "mat3.h"
#include "vec3.h"

// -----------------------------------------------------------------------------
//  Shape types (one BLAS each) and scene objects (one TLAS instance each)
// -----------------------------------------------------------------------------
enum class MeshType : uint8_t {
    Sphere, Diamond, Cube, Pyramid, Cylinder, Dodecahedron, Supertoroid, Supershape,
    Floor,   // not user-addable
    Count
};
inline constexpr std::size_t MESH_COUNT     = std::to_underlying(MeshType::Count);
inline constexpr int         ADDABLE_MESHES = std::to_underlying(MeshType::Floor);

[[nodiscard]] constexpr std::optional<MeshType> addableMesh(int i) noexcept {
    if (i < 0 || i >= ADDABLE_MESHES) return std::nullopt;
    return static_cast<MeshType>(i);
}

// Material ids as the shaders see them (MAT_* in shaders/payload.glsl).
enum class Material : uint32_t { Floor = 0, Solid = 1, Glass = 2, Emissive = 3 };

struct SceneObject {
    MeshType mesh         = MeshType::Sphere;
    Vec3     pos;
    Mat3     rot;
    float    scale        = 1.0f;
    Vec3     color{ 1, 1, 1 };
    Material material     = Material::Glass;   // glass is the showcase material
    float    reflectivity = 0.0f;
    bool     selected     = false;
};
