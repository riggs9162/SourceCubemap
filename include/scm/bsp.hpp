#pragma once
#include "common.hpp"
#include <bsppp/BSP.h>
#include <vpkpp/PackFile.h>

namespace scm {
struct Lump { uint32_t offset, length, version, uncompressed; };
struct Probe { glm::ivec3 origin; uint32_t resolution; std::string name() const; };
struct GameLump { uint16_t version; Bytes bytes; };

class Bsp {
public:
    explicit Bsp(const fs::path& filename);
    Bytes lump(int index) const;
    std::optional<GameLump> gameLump(uint32_t signature) const;
    Json inspect() const;
    void validateTextures() const;
    std::optional<Bytes> embedded(std::string_view name) const;
    void writeCubemaps(const std::map<std::string, Bytes>& textures, const fs::path& output, bool overwrite, bool inPlace) const;
    void verifyPreserved(const Bsp& source, const std::map<std::string, Bytes>& replacements = {}) const;
    fs::path path;
    Bytes bytes;
    std::array<Lump, 64> lumps{};
    std::vector<Probe> probes;
    std::vector<bsppp::BSPEntityKeyValues> entities;
    std::map<std::string, std::string> pakNames;
    std::unique_ptr<bsppp::BSP> source;
private:
    std::unique_ptr<TemporaryFile> pakFile;
    std::unique_ptr<vpkpp::PackFile> pak;
};
}
