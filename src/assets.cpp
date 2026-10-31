#include <scm/assets.hpp>
#include <vpkpp/format/GMA.h>
#include <vpkpp/format/VPK.h>
#include <spdlog/spdlog.h>
#include <unordered_set>

namespace scm {
namespace {
bool indexable(std::string_view name) {
    size_t segment = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/') {
            if (i == segment || name[i - 1] == '.' || name[i - 1] == ' ') { return false; }
            segment = i + 1;
        } else if (static_cast<unsigned char>(name[i]) >= 128) { return false; }
    }
    return true;
}
/// Evaluates a VMT key condition as the desktop engine does: unknown platform symbols are false, and || and && combine terms.
bool conditionHolds(std::string_view condition, bool hdr) {
    auto term = [&](std::string_view text) {
        bool negated = text.starts_with('!');
        if (negated) { text.remove_prefix(1); }
        if (text.starts_with('$')) { text.remove_prefix(1); }
        bool value = text == "win32" || text == "windows" || (text == "hdr" && hdr) || (text == "ldr" && !hdr);
        return value != negated;
    };
    auto all = [&](std::string_view text) {
        for (size_t end = text.find("&&"); ; end = text.find("&&")) {
            if (!term(text.substr(0, end))) { return false; }
            if (end == std::string_view::npos) { return true; }
            text.remove_prefix(end + 2);
        }
    };
    for (size_t end = condition.find("||"); ; end = condition.find("||")) {
        if (all(condition.substr(0, end))) { return true; }
        if (end == std::string_view::npos) { return false; }
        condition.remove_prefix(end + 2);
    }
}
/// Reports whether a DirectX-level fallback block such as >=dx90_20b applies at the desktop level this tool targets.
bool levelHolds(std::string_view key) {
    size_t digits = key.find("dx");
    if (digits == std::string_view::npos) { return false; }
    auto comparison = key.substr(0, digits);
    auto name = key.substr(digits + 2);
    if (comparison != ">=" && comparison != "<" && comparison != "<=" && comparison != ">") { return false; }
    int level = name == "90_20b" ? 92 : name == "90" ? 90 : name == "95" ? 95 : name == "80" ? 80 : name == "81" ? 81 : name == "60" ? 60 : name == "70" ? 70 : -1;
    if (level < 0) { return false; }
    constexpr int current = 95;
    return comparison == ">=" ? current >= level : comparison == ">" ? current > level : comparison == "<=" ? current <= level : current < level;
}
}

/// Answers lookups inside a mounted directory from cached folder listings, so repeated misses cost no disk access.
class Assets::DirectoryIndex {
public:
    explicit DirectoryIndex(fs::path root) { tree.path = std::move(root); }
    bool contains(std::string_view name);
private:
    struct Folder {
        fs::path path;
        bool listed = false;
        std::unordered_set<std::string> files;
        std::unordered_map<std::string, std::unique_ptr<Folder>> folders;
    };
    static void list(Folder& folder);
    Folder tree;
};

bool Assets::DirectoryIndex::contains(std::string_view name) {
    if (!indexable(name)) { return fs::is_regular_file(tree.path / name); }
    auto* folder = &tree;
    for (size_t begin = 0;;) {
        if (!folder->listed) { list(*folder); }
        auto end = name.find('/', begin);
        if (end == std::string_view::npos) { return folder->files.contains(std::string(name.substr(begin))); }
        auto child = folder->folders.find(std::string(name.substr(begin, end - begin)));
        if (child == folder->folders.end()) { return false; }
        folder = child->second.get(); begin = end + 1;
    }
}
void Assets::DirectoryIndex::list(Folder& folder) {
    folder.listed = true;
    std::error_code error;
    for (fs::directory_iterator entry(folder.path, fs::directory_options::skip_permission_denied, error), end; !error && entry != end; entry.increment(error)) {
        auto wide = entry->path().filename().wstring();
        if (std::any_of(wide.begin(), wide.end(), [](wchar_t value) { return value >= 128; })) { continue; }
        std::string name(wide.size(), '\0');
        std::transform(wide.begin(), wide.end(), name.begin(), [](wchar_t value) { return static_cast<char>(value); });
        name = lower(std::move(name));
        std::error_code status;
        if (entry->is_directory(status)) {
            auto child = std::make_unique<Folder>(); child->path = entry->path();
            folder.folders.emplace(std::move(name), std::move(child));
        } else if (entry->is_regular_file(status)) { folder.files.insert(std::move(name)); }
    }
}

void Assets::mount(const fs::path& input, bool required, bool scanArchives) {
    auto path = fs::absolute(input).lexically_normal();
    if (!fs::exists(path)) {
        require(!required, "Mount does not exist: {}", path.string());
        spdlog::debug("Skipped absent optional mount: {}", path.string());
        return;
    }
    if (!mounted.insert(lower(path.string())).second) { return; }
    if (fs::is_directory(path)) {
        sources.push_back({path, {}, std::make_shared<DirectoryIndex>(path)});
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
    require(archive != nullptr, "Cannot open archive: {}", path.string());
    sources.push_back({path, std::move(archive), {}});
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
    for (const auto& source : sources) { declared.push_back(source.path.string()); }
    if (profile == "gmod" && fs::exists(game / "cfg/mount.cfg")) {
        auto text = textOf(readFile(game / "cfg/mount.cfg"));
        kvpp::KV1 config(text);
        if (config.getChildCount()) {
            for (const auto& entry : config[0]) {
                auto path = fs::absolute(fs::path(entry.getValue())).lexically_normal();
                if (!mounted.contains(lower(path.string()))) { declared.push_back(path.string()); deferred.push_back(std::move(path)); }
            }
        }
    }
}
bool Assets::reach(size_t index) {
    while (index >= sources.size() && nextDeferred < deferred.size()) { mount(deferred[nextDeferred++], false); }
    return index < sources.size();
}
std::shared_ptr<const Asset> Assets::remember(const std::string& name, Bytes bytes, std::string origin) {
    auto asset = std::make_shared<Asset>(Asset{std::move(bytes), std::move(origin), {}});
    asset->hash = digest(asset->bytes);
    cache.emplace(name, asset);
    return asset;
}
std::shared_ptr<const Asset> Assets::find(std::string name) {
    name = assetPath(std::move(name));
    if (auto found = cache.find(name); found != cache.end()) { return found->second; }
    if (auto value = bsp.embedded(name)) { return remember(name, std::move(*value), bsp.path.string() + "::" + name); }
    for (size_t index = 0; reach(index); ++index) {
        const auto& source = sources[index];
        std::optional<Bytes> bytes;
        if (source.archive) { bytes = source.archive->readEntry(name); }
        else if (source.files->contains(name)) { bytes = readFile(source.path / name); }
        if (!bytes) { continue; }
        auto asset = remember(name, std::move(*bytes), source.path.string() + "::" + name);
        spdlog::debug("Resolved {} from {}", name, asset->origin);
        return asset;
    }
    cache.emplace(name, nullptr);
    return {};
}
std::shared_ptr<const Asset> Assets::findEmbedded(std::string name) {
    name = assetPath(std::move(name));
    if (auto found = cache.find(name); found != cache.end()) { return found->second; }
    if (auto value = bsp.embedded(name)) { return remember(name, std::move(*value), bsp.path.string() + "::" + name); }
    return {};
}
std::shared_ptr<const Asset> Assets::get(std::string name, std::string_view requestedBy) {
    auto asset = find(name);
    if (asset) { return asset; }
    std::string searched = bsp.path.string() + " (embedded)";
    for (const auto& mount : sources) { searched += "\n  " + mount.path.string(); }
    throw MissingAsset(std::format("Missing asset '{}' requested by '{}'. Searched:\n  {}", name, requestedBy, searched), name);
}
Json Assets::report() const {
    Json result{{"mounts", Json::array()}, {"resolved", Json::array()}, {"unresolved_lookups", Json::array()}};
    for (const auto& path : declared) { result["mounts"].push_back(path); }
    std::vector<std::string> names;
    for (const auto& [name, value] : cache) { names.push_back(name); }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) { const auto& value = cache.at(name); if (value) { result["resolved"].push_back({{"path", name}, {"source", value->origin}, {"hash", value->hash}}); } else { result["unresolved_lookups"].push_back(name); } }
    return result;
}
std::string MaterialDefinition::value(std::string_view key, std::string fallback) const {
    auto found = parameters.find(std::string(key));
    return found == parameters.end() ? fallback : found->second;
}
bool MaterialDefinition::flag(std::string_view key) const {
    auto text = value(key);
    char* end = nullptr;
    double number = std::strtod(text.c_str(), &end);
    return end != text.c_str() && number != 0;
}
namespace {
MaterialDefinition parseMaterial(Assets& assets, const std::string& name, bool hdr, unsigned depth) {
    auto bytes = assets.get(name, "material");
    auto text = textOf(bytes->bytes);
    kvpp::KV1 document(text);
    require(document.getChildCount() == 1, "Invalid VMT root: {}", name);
    const auto& root = document[0];
    MaterialDefinition result;
    result.shader = lower(std::string(root.getKey()));
    auto addProxy = [&](const auto& proxy) {
        result.proxies.emplace_back(proxy.getKey());
        std::map<std::string, std::string> parameters;
        for (const auto& entry : proxy) {
            if (entry.getChildCount() == 0) { parameters[lower(std::string(entry.getKey()))] = std::string(entry.getValue()); }
        }
        result.proxyParameters.push_back(std::move(parameters));
    };
    if (result.shader == "patch") {
        auto include = root["include"].getValue();
        require(!include.empty(), "Patch has no include: {}", name);
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
        if (!conditional.empty() && !conditionHolds(conditional, hdr)) { continue; }
        auto key = lower(std::string(entry.getKey()));
        if (key == "proxies") { for (const auto& proxy : entry) { addProxy(proxy); } }
        else if (entry.getChildCount() == 0) { result.parameters[key] = std::string(entry.getValue()); }
        else if (levelHolds(key) || key == result.shader + "_dx9" || key == result.shader + "_dx90" || key == result.shader + "_dx95" || (hdr && key == result.shader + "_hdr_dx9")) {
            for (const auto& child : entry) {
                auto childKey = lower(std::string(child.getKey()));
                if (childKey == "proxies") { for (const auto& proxy : child) { addProxy(proxy); } }
                else if (child.getChildCount() == 0) { result.parameters[childKey] = std::string(child.getValue()); }
            }
        }
    }
    return result;
}
}
const MaterialDefinition& readMaterial(Assets& assets, std::string name, bool hdr, unsigned depth) {
    require(depth < 32, "VMT include cycle or excessive nesting: {}", name);
    name = assetPath(name);
    if (!name.starts_with("materials/")) { name = "materials/" + name; }
    if (!name.ends_with(".vmt")) { name += ".vmt"; }
    auto& known = assets.materials[hdr ? 1 : 0];
    if (auto found = known.find(name); found != known.end()) { return found->second; }
    auto parsed = parseMaterial(assets, name, hdr, depth);
    return known.emplace(std::move(name), std::move(parsed)).first->second;
}
}
