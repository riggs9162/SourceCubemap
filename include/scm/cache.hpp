#pragma once
#include "scene.hpp"

namespace scm {
void setCacheBudget(uint64_t bytes);
std::string sceneCacheKey(const Bsp& bsp, const Assets& assets, const SceneOptions& options);
std::optional<Scene> readSceneCache(const fs::path& file, Assets& assets);
void writeSceneCache(const fs::path& file, const Scene& scene, const Assets& assets);
void writeCacheFile(const fs::path& file, std::span<const std::byte> bytes);
/// Stores a managed cache file on a background thread and reports failures as warnings. Blocks only while too much data is already waiting.
void queueCacheFile(fs::path file, Bytes bytes);
/// Waits until every queued cache file has been stored.
void flushCacheWrites();
}
