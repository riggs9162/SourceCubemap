#include <scm/assets.hpp>
#include <vpkpp/format/GMA.h>
#include <vpkpp/format/VPK.h>
#include <spdlog/spdlog.h>

namespace scm {
void Assets::mount(const fs::path& input, bool required, bool scanArchives) {
    auto path = fs::absolute(input).lexically_normal();
    if (!fs::exists(path)) {
        require(!required, std::format("Mount does not exist: {}", path.string()));
        spdlog::debug("Skipped absent optional mount: {}", path.string());
        return;
    }
    if (!mounted.insert(lower(path.string())).second) { return; }
    if (fs::is_directory(path)) {
        sources.push_back({path, {}});
        if (!scanArchives) { return; }
        std::vector<fs::path> archives;
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file() && lower(entry.path().filename().string()).ends_with("_dir.vpk")) { archives.push_back(entry.path()); }
        }
        std::sort(archives.rbegin(), archives.rend());
        for (const auto& archive : archives) { mount(archive); }
        return;
    }
    std::unique_ptr<vpkpp::PackFile> archive;
    auto extension = lower(path.extension().string());
    if (extension == ".vpk") { archive = vpkpp::VPK::open(path.string()); }
    else if (extension == ".gma") { archive = vpkpp::GMA::open(path.string()); }
    else { throw Error(std::format("Unsupported archive mount: {}", path.string())); }
    require(archive != nullptr, std::format("Cannot open archive: {}", path.string()));
    sources.push_back({path, std::move(archive)});
}
Assets::Assets(const Bsp& map, const fs::path& game, const std::vector<std::string>& mounts, std::string profile) : bsp(map) {
    require(fs::is_regular_file(game / "gameinfo.txt"), "--game must identify a directory containing gameinfo.txt.");
    for (const auto& path : mounts) { mount(path); }
    if (profile == "gmod" && fs::is_directory(game / "addons")) {
        std::vector<fs::path> addons;
        for (const auto& entry : fs::directory_iterator(game / "addons")) {
            if (entry.is_directory() || lower(entry.path().extension().string()) == ".gma") { addons.push_back(entry.path()); }
        }
        std::sort(addons.begin(), addons.end());
        for (const auto& path : addons) { mount(path); }
    }
    if (profile == "gmod") { mount(game); }
    auto infoText = textOf(readFile(game / "gameinfo.txt"));
    kvpp::KV1 info(infoText);
    require(info.getChildCount() != 0, "gameinfo.txt is empty or invalid.");
    for (const auto& entry : info[0]["FileSystem"]["SearchPaths"]) {
        auto type = lower(std::string(entry.getKey()));
        std::set<std::string> identifiers;
        size_t first = 0;
        while (first < type.size()) { auto last = type.find('+', first); identifiers.insert(type.substr(first, last == std::string::npos ? last : last - first)); if (last == std::string::npos) { break; } first = last + 1; }
        if (!identifiers.contains("game") && !identifiers.contains("mod")) { continue; }
        std::string value(entry.getValue());
        std::replace(value.begin(), value.end(), '\\', '/');
        fs::path resolved;
        if (value.starts_with("|gameinfo_path|")) { resolved = game / value.substr(15); }
        else if (value.starts_with("|all_source_engine_paths|")) { resolved = game.parent_path() / value.substr(25); }
        else { resolved = game.parent_path() / value; }
        if (resolved.filename() == "*") {
            std::vector<fs::path> entries;
            if (fs::is_directory(resolved.parent_path())) {
                for (const auto& entry : fs::directory_iterator(resolved.parent_path())) {
                    auto extension = lower(entry.path().extension().string()), stem = lower(entry.path().stem().string());
                    bool chunk = stem.size() >= 4 && stem[stem.size() - 4] == '_' && std::all_of(stem.end() - 3, stem.end(), [](char value) { return value >= '0' && value <= '9'; });
                    if (entry.is_directory() || (entry.is_regular_file() && ((extension == ".vpk" && !chunk) || extension == ".gma"))) { entries.push_back(entry.path()); }
                }
            }
            std::sort(entries.begin(), entries.end());
            for (const auto& entry : entries) { mount(entry); }
            continue;
        }
        if (resolved.extension() == ".vpk" && !fs::exists(resolved)) { resolved = resolved.parent_path() / (resolved.stem().string() + "_dir.vpk"); }
        mount(resolved, false, false);
    }
    if (profile == "gmod" && fs::exists(game / "cfg/mount.cfg")) {
        auto text = textOf(readFile(game / "cfg/mount.cfg"));
        kvpp::KV1 config(text);
        if (config.getChildCount()) { for (const auto& entry : config[0]) { mount(fs::path(entry.getValue()), false); } }
    }
}
std::shared_ptr<const Asset> Assets::find(std::string name) {
    name = assetPath(std::move(name));
    if (auto found = cache.find(name); found != cache.end()) { return found->second; }
    if (auto value = bsp.embedded(name)) {
        auto asset = std::make_shared<Asset>(Asset{std::move(*value), bsp.path.string() + "::" + name});
        cache.emplace(name, asset);
        return asset;
    }
    for (const auto& source : sources) {
        std::optional<Bytes> bytes;
        if (source.archive) { bytes = source.archive->readEntry(name); }
        else if (fs::is_regular_file(source.path / name)) { bytes = readFile(source.path / name); }
        if (!bytes) { continue; }
        auto asset = std::make_shared<Asset>(Asset{std::move(*bytes), source.path.string() + "::" + name});
        cache.emplace(name, asset);
        spdlog::debug("Resolved {} from {}", name, asset->origin);
        return asset;
    }
    cache.emplace(name, nullptr);
    return {};
}
std::shared_ptr<const Asset> Assets::get(std::string name, std::string_view requestedBy) {
    auto asset = find(name);
    if (asset) { return asset; }
    std::string searched = bsp.path.string() + " (embedded)";
    for (const auto& mount : sources) { searched += "\n  " + mount.path.string(); }
    throw Error(std::format("Missing asset '{}' requested by '{}'. Searched:\n  {}", name, requestedBy, searched));
}
Json Assets::report() const {
    Json result{{"mounts", Json::array()}, {"resolved", Json::array()}, {"unresolved_lookups", Json::array()}};
    for (const auto& source : sources) { result["mounts"].push_back(source.path.string()); }
    std::vector<std::string> names;
    for (const auto& [name, value] : cache) { names.push_back(name); }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) { const auto& value = cache.at(name); if (value) { result["resolved"].push_back({{"path", name}, {"source", value->origin}, {"hash", digest(value->bytes)}}); } else { result["unresolved_lookups"].push_back(name); } }
    return result;
}
std::string MaterialDefinition::value(std::string_view key, std::string fallback) const {
    auto found = parameters.find(std::string(key));
    return found == parameters.end() ? fallback : found->second;
}
bool MaterialDefinition::flag(std::string_view key) const { return parseFloat(value(key), 0) != 0; }
MaterialDefinition readMaterial(Assets& assets, std::string name, bool hdr, unsigned depth) {
    require(depth < 32, std::format("VMT include cycle or excessive nesting: {}", name));
    name = assetPath(name);
    if (!name.starts_with("materials/")) { name = "materials/" + name; }
    if (!name.ends_with(".vmt")) { name += ".vmt"; }
    auto bytes = assets.get(name, "material");
    auto text = textOf(bytes->bytes);
    kvpp::KV1 document(text);
    require(document.getChildCount() == 1, std::format("Invalid VMT root: {}", name));
    const auto& root = document[0];
    MaterialDefinition result;
    result.shader = lower(std::string(root.getKey()));
    if (result.shader == "patch") {
        auto include = root["include"].getValue();
        require(!include.empty(), std::format("Patch has no include: {}", name));
        result = readMaterial(assets, std::string(include), hdr, depth + 1);
        for (const auto& block : root) {
            auto kind = lower(std::string(block.getKey()));
            if (kind == "insert" || kind == "replace") {
                for (const auto& entry : block) {
                    auto key = lower(std::string(entry.getKey()));
                    if ((kind == "insert" && !result.parameters.contains(key)) || (kind == "replace" && result.parameters.contains(key))) { result.parameters[key] = std::string(entry.getValue()); }
                }
            }
        }
        return result;
    }
    for (const auto& entry : root) {
        auto conditional = lower(std::string(entry.getConditional()));
        if (!conditional.empty()) {
            require(conditional == "$win32" || conditional == "$hdr" || conditional == "!$hdr" || conditional == "$ldr", std::format("Unsupported VMT conditional '{}' in {}.", conditional, name));
            if ((conditional == "$hdr" && !hdr) || ((conditional == "!$hdr" || conditional == "$ldr") && hdr)) { continue; }
        }
        auto key = lower(std::string(entry.getKey()));
        if (key == "proxies") { for (const auto& proxy : entry) { result.proxies.emplace_back(proxy.getKey()); } }
        else if (entry.getChildCount() == 0) { result.parameters[key] = std::string(entry.getValue()); }
        else if (key == result.shader + "_hdr_dx9" && !hdr) { continue; }
        else if (key.ends_with("_dx6") || key.ends_with("_dx7") || key.ends_with("_dx8") || key == "<dx80" || key == "<dx81" || key == "<dx90" || key == "<dx95") { continue; }
        else if (key == ">=dx90" || key == ">=dx90_20b" || key == ">=dx95" || key == result.shader + "_dx9" || key == result.shader + "_dx90" || key == result.shader + "_dx95" || (hdr && key == result.shader + "_hdr_dx9")) {
            for (const auto& child : entry) {
                auto childKey = lower(std::string(child.getKey()));
                if (childKey == "proxies") { for (const auto& proxy : child) { result.proxies.emplace_back(proxy.getKey()); } }
                else { require(child.getChildCount() == 0, std::format("Unsupported nested material block '{}' in {}.", childKey, name)); result.parameters[childKey] = std::string(child.getValue()); }
            }
        }
        else { throw Error(std::format("Unsupported material fallback block '{}' in {}.", key, name)); }
    }
    return result;
}
}
