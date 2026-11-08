#include "scene.h"

#include <cmath>
#include <numbers>

#include "config.h"

PrototypeMeshes buildPrototypeMeshes() {
    PrototypeMeshes p;
    MeshBuilder& b = p.geometry;
    auto build = [&](MeshType type, auto&& generate) {
        b.begin();
        generate();
        p.meshes[type] = b.end();
    };
    build(MeshType::Sphere,       [&] { b.sphere(SPHERE_RADIUS); });
    build(MeshType::Diamond,      [&] { b.diamond(1.25f); });
    build(MeshType::Cube,         [&] { b.cube(2.0f); });
    build(MeshType::Pyramid,      [&] { b.pyramid(2.2f, 2.4f); });
    build(MeshType::Cylinder,     [&] { b.cylinder(1.05f, 2.4f, 56); });
    build(MeshType::Dodecahedron, [&] { b.dodecahedron(1.55f); });
    build(MeshType::Supertoroid,  [&] { b.supertoroid(); });
    build(MeshType::Supershape,   [&] { b.supershape(); });
    build(MeshType::Floor,        [&] { b.floor(20.0f); });
    return p;
}

std::vector<SceneObject> defaultScene(const MeshTable& meshes) {
    std::vector<SceneObject> objects;
    auto add = [&](MeshType type, Vec3 pos, Vec3 color, Mat3 rot = {}) {
        objects.push_back({ .mesh = type, .pos = pos, .rot = rot, .color = color, .material = Material::Glass });
    };

    add(MeshType::Supertoroid, { 0.0f, 2.7f, 0.0f }, { 1.0f, 1.0f, 1.0f },
        Mat3::rotation({ 1.0f, 0.25f, 0.35f }, 0.7f));

    constexpr float ringRadius = 6.2f;
    constexpr std::array<std::pair<MeshType, Vec3>, 6> ring{ {
        { MeshType::Sphere,       { 0.55f, 0.72f, 1.00f } },
        { MeshType::Diamond,      { 1.00f, 0.65f, 0.72f } },
        { MeshType::Cube,         { 0.55f, 0.95f, 0.65f } },
        { MeshType::Pyramid,      { 1.00f, 0.85f, 0.45f } },
        { MeshType::Cylinder,     { 0.45f, 0.92f, 0.92f } },
        { MeshType::Dodecahedron, { 0.78f, 0.60f, 1.00f } },
    } };
    for (int k = 0; const auto& [type, color] : ring) {
        const float angle = static_cast<float>(k++) / static_cast<float>(ring.size()) * 2.0f * std::numbers::pi_v<float>;
        add(type, { std::cos(angle) * ringRadius, -meshes[type].minY, std::sin(angle) * ringRadius }, color);   // base on the floor
    }
    return objects;
}
