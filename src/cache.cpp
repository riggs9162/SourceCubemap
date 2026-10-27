#include <scm/cache.hpp>
#include <Windows.h>
#include <spdlog/spdlog.h>

namespace scm {
namespace {
constexpr uint32_t cacheVersion = 9;
Json vector3(glm::vec3 value) { return {value.x, value.y, value.z}; }
Json vector4(glm::vec4 value) { return {value.x, value.y, value.z, value.w}; }
glm::vec3 read3(const Json& value) { return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()}; }
glm::vec4 read4(const Json& value) { return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>(), value.at(3).get<float>()}; }
template<class T> void append(Bytes& output, std::span<const T> input) {
    auto bytes = std::as_bytes(input); output.insert(output.end(), bytes.begin(), bytes.end());
}
template<class T> std::vector<T> readArray(Reader& reader, size_t count) {
    require(count <= (size_t(1) << 30) / sizeof(T), "Cache section exceeds the memory limit.");
    auto bytes = reader.slice(reader.cursor, count * sizeof(T)); reader.cursor += bytes.size();
    std::vector<T> result(count); std::memcpy(result.data(), bytes.data(), bytes.size()); return result;
}
}
std::string sceneCacheKey(const Bsp& bsp, const Assets& assets, const SceneOptions& options) {
    Json key{{"schema", cacheVersion}, {"bsp", digest(bsp.bytes)}, {"mounts", assets.report()["mounts"]}, {"hdr", options.hdr}, {"time", options.time}, {"experimental", options.experimental}, {"default_sky", options.defaultSky}};
    auto text = key.dump(); return digest(std::as_bytes(std::span(text.data(), text.size())));
}
void writeCacheFile(const fs::path& file, std::span<const std::byte> bytes) {
    fs::create_directories(file.parent_path()); TemporaryFile temporary(".cache", file.parent_path());
    writeFile(temporary.path, bytes);
    require(MoveFileExW(temporary.path.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0, "Cannot publish a cache file.");
}
void writeSceneCache(const fs::path& file, const Scene& scene, const Assets& assets) {
    Json metadata{{"schema", cacheVersion}, {"vertex_stride", sizeof(Vertex)}, {"vertices", scene.vertices.size()}, {"indices", scene.indices.size()}, {"hdr", scene.hdr}, {"sky_origin", vector3(scene.skyOrigin)}, {"sky_scale", scene.skyScale}, {"fog_color", vector3(scene.fogColor)}, {"fog_start", scene.fogStart}, {"fog_end", scene.fogEnd}, {"sky_fog_color", vector3(scene.skyFogColor)}, {"sky_fog_start", scene.skyFogStart}, {"sky_fog_end", scene.skyFogEnd}, {"diagnostics", scene.diagnostics}, {"assets", assets.report()}, {"materials", Json::array()}, {"draws", Json::array()}, {"images", Json::array()}, {"probes", Json::array()}};
    for (const auto& material : scene.materials) {
        metadata["materials"].push_back({{"name", material.name}, {"shader", material.definition.shader}, {"parameters", material.definition.parameters}, {"proxies", material.definition.proxies}, {"textures", {material.base, material.second, material.normal, material.detail}}, {"flags", material.flags}, {"tint", vector4(material.tint)}, {"alpha", material.alphaReference}, {"detail", {material.detailScale, material.detailFactor, float(material.detailMode)}}, {"translucent", material.translucent}, {"additive", material.additive}, {"no_cull", material.noCull}, {"no_fog", material.noFog}});
    }
    for (const auto& draw : scene.draws) { metadata["draws"].push_back({{"first", draw.firstIndex}, {"count", draw.indexCount}, {"material", draw.material}, {"lightmaps", draw.lightmaps}, {"sky_world", draw.skyWorld}, {"sky_box", draw.skyBox}, {"visibility", draw.visibility}, {"center", vector3(draw.center)}, {"radius", draw.radius}}); }
    for (const auto& image : scene.images) { metadata["images"].push_back({{"name", image.name}, {"width", image.width}, {"height", image.height}, {"srgb", image.srgb}, {"clamp", image.clamp}}); }
    for (const auto& probe : scene.probes) { metadata["probes"].push_back({{"origin", {probe.origin.x, probe.origin.y, probe.origin.z}}, {"resolution", probe.resolution}}); }
    Bytes payload; append(payload, std::span(scene.vertices)); append(payload, std::span(scene.indices));
    for (const auto& image : scene.images) { append(payload, std::span(image.pixels)); }
    metadata["payload_hash"] = digest(payload);
    auto description = metadata.dump();
    Bytes bytes; std::array<uint32_t, 3> header{0x434d4353, cacheVersion, static_cast<uint32_t>(description.size())};
    append(bytes, std::span<const uint32_t>(header)); append(bytes, std::span<const char>(description)); append(bytes, std::span<const std::byte>(payload));
    writeCacheFile(file, bytes);
}
std::optional<Scene> readSceneCache(const fs::path& file, Assets& assets) {
    if (!fs::exists(file)) { return {}; }
    try {
        auto data = readFile(file); Reader reader(data, "scene cache");
        if (reader.take<uint32_t>() != 0x434d4353 || reader.take<uint32_t>() != cacheVersion) { return {}; }
        auto descriptionLength = reader.take<uint32_t>();
        auto description = reader.slice(reader.cursor, descriptionLength); reader.cursor += descriptionLength;
        auto metadata = Json::parse(textOf(description));
        require(metadata.at("vertex_stride").get<size_t>() == sizeof(Vertex), "Cache vertex layout changed.");
        if (metadata.at("assets").at("mounts") != assets.report().at("mounts")) { return {}; }
        for (const auto& dependency : metadata.at("assets").at("resolved")) {
            auto current = assets.find(dependency.at("path").get<std::string>());
            if (!current || current->origin != dependency.at("source").get<std::string>() || digest(current->bytes) != dependency.at("hash").get<std::string>()) { return {}; }
        }
        require(digest(reader.slice(reader.cursor, data.size() - reader.cursor)) == metadata.at("payload_hash").get<std::string>(), "Scene cache payload hash differs.");
        Scene scene;
        scene.vertices = readArray<Vertex>(reader, metadata.at("vertices")); scene.indices = readArray<uint32_t>(reader, metadata.at("indices"));
        for (const auto& item : metadata.at("images")) {
            Image image; image.name = item.at("name"); image.width = item.at("width"); image.height = item.at("height"); image.srgb = item.at("srgb"); image.clamp = item.at("clamp");
            require(image.width > 0 && image.height > 0 && image.width <= 32768 && image.height <= 32768, "Invalid cached image dimensions.");
            image.pixels = readArray<glm::vec4>(reader, size_t(image.width) * image.height); scene.images.push_back(std::move(image));
        }
        require(reader.cursor == data.size(), "Unexpected bytes after cached scene data.");
        for (const auto& item : metadata.at("materials")) {
            Material material; material.name = item.at("name"); material.definition.shader = item.at("shader");
            material.definition.parameters = item.at("parameters").get<std::map<std::string, std::string>>(); material.definition.proxies = item.at("proxies").get<std::vector<std::string>>();
            material.base = item.at("textures").at(0); material.second = item.at("textures").at(1); material.normal = item.at("textures").at(2); material.detail = item.at("textures").at(3);
            for (auto image : {material.base, material.second, material.normal, material.detail}) { require(image < scene.images.size(), "Invalid cached material texture."); }
            material.flags = item.at("flags"); material.tint = read4(item.at("tint")); material.alphaReference = item.at("alpha"); material.detailScale = item.at("detail").at(0); material.detailFactor = item.at("detail").at(1); material.detailMode = int(item.at("detail").at(2).get<float>());
            material.translucent = item.at("translucent"); material.additive = item.at("additive"); material.noCull = item.at("no_cull"); material.noFog = item.at("no_fog"); scene.materials.push_back(std::move(material));
        }
        for (const auto& item : metadata.at("draws")) {
            Draw draw; draw.firstIndex = item.at("first"); draw.indexCount = item.at("count"); draw.material = item.at("material"); draw.lightmaps = item.at("lightmaps").get<std::array<uint32_t, 4>>();
            draw.skyWorld = item.at("sky_world"); draw.skyBox = item.at("sky_box"); draw.visibility = item.at("visibility").get<std::vector<uint64_t>>(); draw.center = read3(item.at("center")); draw.radius = item.at("radius");
            require(uint64_t(draw.firstIndex) + draw.indexCount <= scene.indices.size() && draw.material < scene.materials.size(), "Invalid cached draw range.");
            for (auto image : draw.lightmaps) { require(image < scene.images.size(), "Invalid cached lightmap."); } scene.draws.push_back(std::move(draw));
        }
        for (const auto& item : metadata.at("probes")) { scene.probes.push_back({{item.at("origin").at(0).get<int>(), item.at("origin").at(1).get<int>(), item.at("origin").at(2).get<int>()}, item.at("resolution")}); }
        for (auto index : scene.indices) { require(index < scene.vertices.size(), "Invalid cached geometry index."); }
        scene.hdr = metadata.at("hdr"); scene.skyOrigin = read3(metadata.at("sky_origin")); scene.skyScale = metadata.at("sky_scale");
        scene.fogColor = read3(metadata.at("fog_color")); scene.fogStart = metadata.at("fog_start"); scene.fogEnd = metadata.at("fog_end");
        scene.skyFogColor = read3(metadata.at("sky_fog_color")); scene.skyFogStart = metadata.at("sky_fog_start"); scene.skyFogEnd = metadata.at("sky_fog_end"); scene.diagnostics = metadata.at("diagnostics");
        scene.diagnostics["scene_cache_hit"] = true;
        return scene;
    } catch (const std::exception& error) { spdlog::warn("Ignoring invalid scene cache: {}", error.what()); return {}; }
}
}
