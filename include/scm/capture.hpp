#pragma once
#include "scene.hpp"

namespace scm {
struct Cube {
    uint32_t resolution = 0;
    std::array<std::vector<glm::vec4>, 6> faces;
};
glm::vec3 cubeDirection(unsigned face, glm::vec2 uv);
glm::vec4 sampleCube(const Cube& cube, glm::vec3 direction);
Bytes encodeCube(const Cube& cube, bool hdr);
void writePreview(const Cube& cube, const fs::path& directory, std::string_view name);

struct RenderOptions {
    std::string device;
    std::string quality = "standard";
    fs::path shaderDirectory;
    fs::path cacheDirectory;
};
class Renderer {
public:
    explicit Renderer(const RenderOptions& options);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void prepare(const Scene& scene);
    Cube capture(const Scene& scene, const Probe& probe, bool skyOnly = false);
    Json statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
