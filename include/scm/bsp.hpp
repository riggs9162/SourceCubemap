#pragma once
#include "common.hpp"
#include <bsppp/BSP.h>
#include <vpkpp/PackFile.h>
#include <mutex>

namespace scm {
struct Lump { uint32_t offset, length, version, uncompressed; };
struct Probe { glm::ivec3 origin; uint32_t resolution; std::string name() const; };
struct GameLump { uint16_t version; Bytes bytes; };
using TextureMap = std::map<std::string, std::span<const std::byte>>;
void validateVtf(std::span<const std::byte> data, std::string_view name);

class Bsp {
public:
    explicit Bsp(const fs::path& filename);
    Bytes lump(int index) const;
    /// Returns a decoded lump without copying it; compressed lumps are decoded once and kept with the map.
    std::span<const std::byte> view(int index) const;
    /// Decodes the listed compressed lumps together on worker threads so later views are immediate.
    void decodeLumps(std::span<const int> indices) const;
    std::optional<GameLump> gameLump(uint32_t signature) const;
    Json inspect() const;
    void validateTextures() const;
    void validateDestination(const fs::path& output, bool overwrite, bool inPlace) const;
    std::optional<Bytes> embedded(std::string_view name) const;
    /// Publishes a copy with the textures embedded and returns the digest of the bytes that passed validation, so callers can confirm the published file.
    std::string writeCubemaps(const TextureMap& textures, const fs::path& output, bool overwrite, bool inPlace, bool backup = true) const;
    void verifyPreserved(const Bsp& source, const TextureMap& replacements = {}) const;
    fs::path path;
    Bytes bytes;
    std::array<Lump, 64> lumps{};
    std::vector<Probe> probes;
    std::vector<bsppp::BSPEntityKeyValues> entities;
    std::map<std::string, std::string> pakNames;
    std::unique_ptr<bsppp::BSP> source;
private:
    std::map<std::string, Bytes> pakData;
    mutable std::map<int, Bytes> decoded;
    mutable std::mutex decodedLock;
};
}
