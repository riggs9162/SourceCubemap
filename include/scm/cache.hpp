#pragma once
#include "scene.hpp"

namespace scm {
void setCacheBudget(uint64_t bytes);
std::string sceneCacheKey(const Bsp& bsp, const Assets& assets, const SceneOptions& options);
std::optional<Scene> readSceneCache(const fs::path& file, Assets& assets);
void writeSceneCache(const fs::path& file, const Scene& scene, const Assets& assets);
void writeCacheFile(const fs::path& file, std::span<const std::byte> bytes);
}
