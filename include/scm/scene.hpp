#pragma once
#include "assets.hpp"

namespace scm {
enum class PixelFormat : uint32_t { RGBA8, BGRA8, BC1, BC2, BC3, RGBA16F, RGBA32F };
/// A GPU-ready texture: a tightly packed mip chain, largest level first, stored in its upload format.
struct Image {
    std::string name;
    uint32_t width = 1, height = 1;
    bool srgb = false;
    bool clamp = false;
    Bytes pixels;
    uint32_t mipCount = 1;
    PixelFormat format = PixelFormat::RGBA16F;
};
/// Returns the byte length of one mip level; block formats round up to whole 4x4 blocks.
size_t pixelBytes(PixelFormat format, uint32_t width, uint32_t height);
/// Returns a one-pixel image of a linear color.
Image solidImage(std::string name, glm::vec4 color);
struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{0, 0, 1};
    glm::vec4 tangent{1, 0, 0, 1};
    glm::vec2 uv{};
    glm::vec2 lightUV{};
    glm::vec4 color{1};
    float blend = 0;
    glm::vec4 secondaryUV{};
};
enum MaterialFlags : uint32_t {
    Unlit = 1, Blend = 2, AlphaTest = 4, NormalMap = 8, BumpLight = 16,
    SelfIllum = 32, Detail = 64, VertexLit = 128, Sky = 256, VertexAlpha = 512, SSBump = 1024,
    Painted = 2048, Phong = 4096, HalfLambert = 8192, PhongBaseAlpha = 16384,
    PhongInvertMask = 32768, PhongAlbedoTint = 65536, PhongExponentTexture = 131072, BlendTintByAlpha = 262144, Water = 524288, BlendModulation = 1048576, SelfIllumMask = 2097152, TwoTexture = 4194304, Modulate = 8388608, Modulate2x = 16777216, Omitted = 0x80000000u
};
struct PaintedSky {
    bool enabled = false;
    glm::vec3 top{0.2f, 0.5f, 1}, bottom{0.8f, 1, 1};
    glm::vec3 sunDirection{0.4f, 0, 0.01f}, sunColor{0.2f, 0.1f, 0}, duskColor{1, 0.2f, 0};
    float fadeBias = 1, hdrScale = 0.66f, sunSize = 2, duskScale = 1, duskIntensity = 1;
    uint32_t stars = 0, starLayers = 1;
    float starScale = 0.5f, starFade = 1.5f, starPosition = 0;
};
struct PropLight {
    glm::vec4 positionType{};
    glm::vec4 color{};
    glm::vec4 attenuation{};
    glm::vec4 directionCone{};
};
struct PropLighting {
    std::array<glm::vec3, 6> ambient{};
    std::array<PropLight, 4> lights{};
    uint32_t count = 0;
    bool baked = false;
    bool lightmapped = false;
};
struct WaterMaterial {
    glm::vec3 fogColor{}, reflectTint{1}, refractTint{1}, scroll1{}, scroll2{};
    glm::vec2 scroll{}, scale{1};
    float fogStart = 0, fogEnd = 1024, reflectAmount = 0.8f, refractAmount = 0;
    bool above = true, reflection = true, reflectEntities = false, noFresnel = false;
};
struct Material {
    std::string name;
    MaterialDefinition definition;
    uint32_t base = 0, second = 0, normal = 1, detail = 0;
    uint32_t blendMask = 0;
    uint32_t selfIllumMask = 0;
    uint32_t phongTexture = 0;
    glm::vec3 selfIllumTint{1};
    uint32_t flags = 0;
    glm::vec4 tint{1};
    float alphaReference = 0.5f, detailScale = 4, detailFactor = 1;
    int detailMode = 0;
    float phongExponent = 5, phongBoost = 1, phongExponentFactor = 149;
    float blendTintOverBase = 0;
    glm::vec3 phongFresnel{0, 0.5f, 1}, phongTint{1};
    bool translucent = false, additive = false, noCull = false, noFog = false;
    WaterMaterial water;
};
struct Draw {
    uint32_t firstIndex = 0, indexCount = 0, material = 0;
    std::array<uint32_t, 4> lightmaps{};
    bool skyWorld = false, skyBox = false;
    std::vector<uint64_t> visibility;
    glm::vec3 center{};
    float radius = 0;
    uint32_t lighting = 0;
};
struct Scene {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Material> materials;
    std::vector<Image> images;
    std::vector<Draw> draws;
    std::vector<Probe> probes;
    PaintedSky paintedSky;
    std::vector<PropLighting> propLighting{PropLighting{}};
    glm::vec3 skyOrigin{};
    float skyScale = 0;
    glm::vec3 fogColor{};
    float fogStart = 0, fogEnd = 0;
    glm::vec3 skyFogColor{};
    float skyFogStart = 0, skyFogEnd = 0;
    bool hdr = false;
    bool hasWater = false;
    float waterHeight = 0;
    uint32_t waterMaterial = 0;
    std::vector<uint8_t> submergedProbes;
    double captureTime = 0;
    Json diagnostics;
};
struct SceneOptions { bool hdr = false; double time = 0; fs::path cache; bool useCache = true; bool experimental = false; bool defaultSky = false; bool planarWater = false; std::string profile = "gmod"; };
Scene loadScene(const Bsp& bsp, Assets& assets, const SceneOptions& options);
}
