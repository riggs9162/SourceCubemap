#pragma once
#include "scene.hpp"

namespace scm {
struct VertexLightingMesh {
    uint32_t lod = 0;
    std::vector<glm::vec3> colors;
};
std::vector<VertexLightingMesh> readVertexLighting(std::span<const std::byte> bytes, uint32_t checksum);
std::array<float, 256> initialLightStyles(const Bsp& bsp);

class WorldLighting {
public:
    WorldLighting(const Bsp& bsp, bool hdr);
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
    Bytes nodes, planes, leaves, brushes, brushSides, leafBrushes, texInfo;
};
}
