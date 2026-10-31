#pragma once
#include "scene.hpp"

namespace scm {
struct VertexLightingMesh {
    uint32_t lod = 0;
    std::vector<glm::vec3> colors;
};
std::vector<VertexLightingMesh> readVertexLighting(std::span<const std::byte> bytes, uint32_t checksum);
/// Returns each light style's brightness at capture start. Strict mode refuses animated styles; experimental mode uses their first frame and reports it through warnings.
std::array<float, 256> initialLightStyles(const Bsp& bsp, bool experimental, std::vector<std::string>* warnings = nullptr);
std::array<glm::vec3, 4> lightmapRadiance(std::array<glm::vec3, 4> samples, bool bumped, bool hdr);

class WorldLighting {
public:
    WorldLighting(const Bsp& bsp, bool hdr, bool experimental);
    PropLighting sample(glm::vec3 position) const;
private:
    struct Light {
        glm::vec3 origin{}, intensity{}, normal{};
        int type = 0, style = 0, flags = 0;
        float inner = 0, outer = 0, exponent = 0, radius = 0;
        glm::vec3 attenuation{};
    };
    bool visible(glm::vec3 start, glm::vec3 end, bool sky) const;
    std::vector<Light> lights;
    std::span<const std::byte> nodes, planes, leaves, brushes, brushSides, leafBrushes, texInfo;
};
}
