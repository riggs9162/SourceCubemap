#pragma once
#include "assets.hpp"

namespace scm {
struct Image {
    std::string name;
    uint32_t width = 1, height = 1;
    bool srgb = false;
    bool clamp = false;
    std::vector<glm::vec4> pixels{glm::vec4(1)};
};
struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0, 0, 1};
    glm::vec4 tangent{1, 0, 0, 1};
    glm::vec2 uv{};
    glm::vec2 lightUV{};
    glm::vec4 color{1};
    float blend = 0;
};
enum MaterialFlags : uint32_t {
    Unlit = 1, Blend = 2, AlphaTest = 4, NormalMap = 8, BumpLight = 16,
    SelfIllum = 32, Detail = 64, VertexLit = 128, Sky = 256
};
struct Material {
    std::string name;
    MaterialDefinition definition;
    uint32_t base = 0, second = 0, normal = 1, detail = 0;
    uint32_t flags = 0;
    glm::vec4 tint{1};
    float alphaReference = 0.5f, detailScale = 4, detailFactor = 1;
    int detailMode = 0;
    bool translucent = false, additive = false, noCull = false;
};
struct Draw {
    uint32_t firstIndex = 0, indexCount = 0, material = 0;
    std::array<uint32_t, 4> lightmaps{};
    bool skyWorld = false, skyBox = false;
};
struct Scene {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Material> materials;
    std::vector<Image> images;
    std::vector<Draw> draws;
    glm::vec3 skyOrigin{};
    float skyScale = 0;
    glm::vec3 fogColor{};
    float fogStart = 0, fogEnd = 0;
    bool hdr = false;
    Json diagnostics;
};
struct SceneOptions { bool hdr = false; double time = 0; fs::path cache; bool useCache = true; bool experimental = false; };
Scene loadScene(const Bsp& bsp, Assets& assets, const SceneOptions& options);
}
