#pragma once
#include "bsp.hpp"
#include <kvpp/KV1.h>
#include <unordered_map>
#include <set>

namespace scm {
struct Asset { Bytes bytes; std::string origin; std::string hash; };
struct MaterialDefinition {
    std::string shader;
    std::map<std::string, std::string> parameters;
    std::vector<std::string> proxies;
    std::vector<std::map<std::string, std::string>> proxyParameters;
    std::string value(std::string_view key, std::string fallback = {}) const;
    /// Reads an on/off parameter the way the engine does: text that is not a number counts as off.
    bool flag(std::string_view key) const;
};
class Assets {
public:
    Assets(const Bsp& map, const fs::path& game, const std::vector<std::string>& mounts, std::string profile);
    std::shared_ptr<const Asset> find(std::string name);
    /// Resolves a file from the map's embedded archive alone, for files that only ever ship inside the map.
    std::shared_ptr<const Asset> findEmbedded(std::string name);
    std::shared_ptr<const Asset> get(std::string name, std::string_view requestedBy);
    Json report() const;
    void mount(const fs::path& path, bool required = true, bool scanArchives = true);
private:
    class DirectoryIndex;
    struct Mount { fs::path path; std::unique_ptr<vpkpp::PackFile> archive; std::shared_ptr<DirectoryIndex> files; };
    bool reach(size_t index);
    std::shared_ptr<const Asset> remember(const std::string& name, Bytes bytes, std::string origin);
    friend const MaterialDefinition& readMaterial(Assets& assets, std::string name, bool hdr, unsigned depth);
    const Bsp& bsp;
    std::vector<Mount> sources;
    std::set<std::string> mounted;
    std::vector<std::string> declared;
    std::vector<fs::path> deferred;
    size_t nextDeferred = 0;
    std::unordered_map<std::string, std::shared_ptr<const Asset>> cache;
    std::array<std::unordered_map<std::string, MaterialDefinition>, 2> materials;
};
/// Returns a material parsed once per lighting variant; the reference stays valid for the life of the assets.
const MaterialDefinition& readMaterial(Assets& assets, std::string name, bool hdr, unsigned depth = 0);
}
