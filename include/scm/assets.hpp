#pragma once
#include "bsp.hpp"
#include <kvpp/KV1.h>
#include <unordered_map>
#include <set>

namespace scm {
struct Asset { Bytes bytes; std::string origin; };
class Assets {
public:
    Assets(const Bsp& map, const fs::path& game, const std::vector<std::string>& mounts, std::string profile);
    std::shared_ptr<const Asset> find(std::string name);
    std::shared_ptr<const Asset> get(std::string name, std::string_view requestedBy);
    Json report() const;
    void mount(const fs::path& path, bool required = true, bool scanArchives = true);
private:
    struct Mount { fs::path path; std::unique_ptr<vpkpp::PackFile> archive; };
    const Bsp& bsp;
    std::vector<Mount> sources;
    std::set<std::string> mounted;
    std::unordered_map<std::string, std::shared_ptr<const Asset>> cache;
};
struct MaterialDefinition {
    std::string shader;
    std::map<std::string, std::string> parameters;
    std::vector<std::string> proxies;
    std::string value(std::string_view key, std::string fallback = {}) const;
    bool flag(std::string_view key) const;
};
MaterialDefinition readMaterial(Assets& assets, std::string name, bool hdr, unsigned depth = 0);
}
