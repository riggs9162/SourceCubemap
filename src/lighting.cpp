#include <scm/lighting.hpp>
#include <algorithm>

namespace scm {
namespace {
const auto legacyLightLevels = [] {
    std::array<float, 4096> levels{};
    for (size_t i = 0; i < levels.size(); ++i) { levels[i] = std::pow(float(i) / 1024.0f, 1.0f / 2.2f) * 0.5f; }
    return levels;
}();
const auto decodedLightLevels = [] {
    std::array<float, 256> levels{};
    for (size_t i = 0; i < levels.size(); ++i) {
        float value = float(i) / 255.0f;
        levels[i] = (value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f)) * std::pow(2.0f, 2.2f);
    }
    return levels;
}();
const auto vertexLightLevels = [] {
    std::array<float, 256> levels{};
    for (size_t i = 0; i < levels.size(); ++i) { levels[i] = std::pow(float(i) * (2.0f / 255.0f), 2.2f); }
    return levels;
}();
float legacyLightEncoding(float value) {
    float level = std::round(std::clamp(value, 0.0f, 4095.0f / 1024.0f) * 1024.0f);
    return std::isnan(level) ? level : legacyLightLevels[size_t(level)];
}
glm::vec3 decodeLightmap(glm::vec3 encoded) {
    encoded = glm::round(glm::clamp(encoded, glm::vec3(0), glm::vec3(1)) * 255.0f);
    for (unsigned channel = 0; channel < 3; ++channel) {
        if (!std::isnan(encoded[channel])) { encoded[channel] = decodedLightLevels[size_t(encoded[channel])]; }
    }
    return encoded;
}
float largest(glm::vec3 color) { return std::max({color.r, color.g, color.b}); }
}

std::array<glm::vec3, 4> lightmapRadiance(std::array<glm::vec3, 4> samples, bool bumped, bool hdr) {
    if (hdr) {
        if (bumped) {
            auto sum = samples[1] + samples[2] + samples[3];
            for (unsigned channel = 0; channel < 3; ++channel) {
                float correction = sum[channel] > 0 ? samples[0][channel] * 3.0f / sum[channel] : 0;
                for (unsigned plane = 1; plane < 4; ++plane) { samples[plane][channel] *= correction; }
            }
        }
        return samples;
    }
    glm::vec3 target;
    for (unsigned channel = 0; channel < 3; ++channel) { target[channel] = legacyLightEncoding(samples[0][channel]); }
    if (bumped) {
        auto sum = samples[1] + samples[2] + samples[3];
        for (unsigned channel = 0; channel < 3; ++channel) {
            float correction = sum[channel] > 0 ? target[channel] * 3.0f / sum[channel] : 0;
            for (unsigned plane = 1; plane < 4; ++plane) { samples[plane][channel] *= correction; }
        }
        std::array<unsigned, 3> order{1, 2, 3};
        std::sort(order.begin(), order.end(), [&](auto a, auto b) { return largest(samples[a]) > largest(samples[b]); });
        for (auto plane : order) {
            float maximum = largest(samples[plane]);
            if (maximum <= 1) { continue; }
            auto clipped = samples[plane] / maximum;
            auto excess = (samples[plane] - clipped) * 0.5f;
            samples[plane] = clipped;
            for (unsigned other = 1; other < 4; ++other) { if (other != plane) { samples[other] += excess; } }
        }
        for (unsigned plane = 1; plane < 4; ++plane) { samples[plane] = decodeLightmap(samples[plane] / std::max(largest(samples[plane]), 1.0f)); }
    } else {
        samples.fill(decodeLightmap(target));
        return samples;
    }
    samples[0] = decodeLightmap(target);
    return samples;
}

std::array<float, 256> initialLightStyles(const Bsp& bsp, bool experimental, std::vector<std::string>* warnings) {
    static constexpr std::array<std::string_view, 13> presets{"m", "mmnmmommommnonmmonqnmmo", "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba", "mmmmmaaaaammmmmaaaaaabcdefgabcdefg", "mamamamamama", "jklmnopqrstuvwxyzyxwvutsrqponmlkj", "nmonqnmomnmomomno", "mmmaaaabcdefgmmmmaaaammmaamm", "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa", "aaaaaaaazzzzzzzz", "mmamammmmammamamaaamammma", "abcdefghijklmnopqrrqponmlkjihgfedcba", "mmnnmmnnnmmnn"};
    std::array<float, 256> result; result.fill(1.0f);
    for (const auto& entity : bsp.entities) {
        auto type = entity["classname"].getValue();
        if (type != "light" && type != "light_spot" && type != "light_environment") { continue; }
        int style = static_cast<int>(parseFloat(entity["style"].getValue()));
        if (style <= 0 || style >= 255) { continue; }
        auto pattern = std::string(entity["pattern"].getValue());
        int preset = static_cast<int>(parseFloat(entity["defaultstyle"].getValue()));
        bool off = (static_cast<int>(parseFloat(entity["spawnflags"].getValue())) & 1) != 0;
        if (pattern.empty() && preset > 0 && preset < 13) { pattern = presets[preset]; }
        if (!off && pattern.size() > 1) {
            require(experimental, "Animated light style {} requires unsupported runtime evaluation. Use --experimental to capture its first frame.", style);
            if (warnings) { warnings->push_back(std::format("Animated light style {} is captured at its first frame.", style)); }
        }
        char first = pattern.empty() ? 'm' : static_cast<char>(std::tolower(static_cast<unsigned char>(pattern.front())));
        result[style] = off ? 0.0f : std::clamp(first - 'a', 0, 25) / 12.0f;
    }
    return result;
}

std::vector<VertexLightingMesh> readVertexLighting(std::span<const std::byte> bytes, uint32_t checksum) {
    Reader reader(bytes, "VHV vertex lighting");
    require(reader.at<uint32_t>(0) == 2, "Only VHV version 2 is supported.");
    require(reader.at<uint32_t>(4) == checksum, "VHV lighting checksum differs from its model.");
    require(reader.at<uint32_t>(8) == 4 && reader.at<uint32_t>(12) == 4, "VHV lighting must use four-byte vertex colors.");
    auto vertices = reader.at<uint32_t>(16), meshes = reader.at<uint32_t>(20);
    require(meshes <= 65536 && vertices <= (1u << 24), "VHV vertex or mesh count exceeds the supported limit.");
    size_t headerEnd = 40 + size_t(meshes) * 28;
    reader.slice(0, headerEnd);
    std::vector<VertexLightingMesh> result;
    std::vector<std::pair<size_t, size_t>> ranges;
    uint64_t total = 0;
    for (uint32_t i = 0; i < meshes; ++i) {
        size_t record = 40 + size_t(i) * 28;
        VertexLightingMesh mesh; mesh.lod = reader.at<uint32_t>(record);
        auto count = reader.at<uint32_t>(record + 4), offset = reader.at<uint32_t>(record + 8);
        require(mesh.lod < 8 && count <= vertices && offset >= headerEnd, "Invalid VHV mesh range or LOD.");
        auto pixels = reader.slice(offset, size_t(count) * 4).data();
        if (count) { ranges.emplace_back(offset, size_t(offset) + size_t(count) * 4); }
        total += count;
        mesh.colors.resize(count);
        for (uint32_t j = 0; j < count; ++j) {
            const auto* pixel = pixels + size_t(j) * 4;
            mesh.colors[j] = {vertexLightLevels[uint8_t(pixel[2])], vertexLightLevels[uint8_t(pixel[1])], vertexLightLevels[uint8_t(pixel[0])]};
        }
        result.push_back(std::move(mesh));
    }
    require(total == vertices, "VHV mesh vertex counts do not match the file header.");
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) { require(ranges[i].first >= ranges[i - 1].second, "VHV mesh vertex ranges overlap."); }
    return result;
}

WorldLighting::WorldLighting(const Bsp& bsp, bool hdr, bool experimental)
    : nodes(bsp.view(5)), planes(bsp.view(1)), leaves(bsp.view(10)), brushes(bsp.view(18)), brushSides(bsp.view(19)), leafBrushes(bsp.view(17)), texInfo(bsp.view(6)) {
    int id = hdr ? 54 : 15;
    auto styles = initialLightStyles(bsp, experimental);
    auto bytes = bsp.view(id); Reader reader(bytes, "world lights");
    require(bsp.lumps[id].version == 0 && bytes.size() % 88 == 0, "Unsupported world-light record layout.");
    require(brushes.size() % 12 == 0 && brushSides.size() % 8 == 0 && leafBrushes.size() % 2 == 0, "Unsupported lighting trace brush layout.");
    for (size_t offset = 0; offset < bytes.size(); offset += 88) {
        Light light;
        light.origin = reader.at<glm::vec3>(offset); light.intensity = reader.at<glm::vec3>(offset + 12); light.normal = reader.at<glm::vec3>(offset + 24);
        light.type = reader.at<int32_t>(offset + 40); light.style = reader.at<int32_t>(offset + 44);
        light.inner = reader.at<float>(offset + 48); light.outer = reader.at<float>(offset + 52); light.exponent = reader.at<float>(offset + 56); light.radius = reader.at<float>(offset + 60);
        light.attenuation = reader.at<glm::vec3>(offset + 64); light.flags = reader.at<int32_t>(offset + 76);
        require(light.type >= 0 && light.type <= 5, "Unsupported world-light type.");
        require(light.style >= 0 && light.style < 255, "World light has an invalid light style.");
        light.intensity *= styles[light.style];
        for (float value : {light.origin.x, light.origin.y, light.origin.z, light.intensity.x, light.intensity.y, light.intensity.z, light.normal.x, light.normal.y, light.normal.z, light.attenuation.x, light.attenuation.y, light.attenuation.z, light.inner, light.outer, light.radius, light.exponent}) { require(std::isfinite(value), "Nonfinite world-light data."); }
        if (light.radius < 0 || light.exponent < 0 || !glm::all(glm::greaterThanEqual(light.intensity, glm::vec3(0))) || !glm::all(glm::greaterThanEqual(light.attenuation, glm::vec3(0)))) { continue; }
        lights.push_back(light);
    }
}

bool WorldLighting::visible(glm::vec3 start, glm::vec3 end, bool sky) const {
    Reader nodeReader(nodes, "lighting trace nodes"), planeReader(planes, "lighting trace planes"), leafReader(leaves, "lighting trace leaves");
    Reader brushReader(brushes, "lighting trace brushes"), sideReader(brushSides, "lighting trace sides"), referenceReader(leafBrushes, "lighting trace brush references"), textureReader(texInfo, "lighting trace textures");
    struct Segment { int32_t node; float begin, end; };
    thread_local std::vector<Segment> stack;
    thread_local std::vector<uint32_t> checkedBrushes;
    thread_local uint32_t trace = 0;
    stack.assign(1, {0, 0, 1});
    if (checkedBrushes.size() < brushes.size() / 12) { checkedBrushes.assign(brushes.size() / 12, 0); trace = 0; }
    if (++trace == 0) { std::fill(checkedBrushes.begin(), checkedBrushes.end(), 0u); trace = 1; }
    float nearest = 1;
    bool hitSky = false;
    size_t visits = 0;
    while (!stack.empty()) {
        require(++visits <= nodes.size() / 32 * 4 + leaves.size() / 32 + 1, "Cycle in lighting trace nodes.");
        auto segment = stack.back(); stack.pop_back();
        if (segment.node >= 0) {
            size_t node = size_t(segment.node) * 32;
            size_t plane = size_t(nodeReader.at<uint32_t>(node)) * 20;
            auto normal = planeReader.at<glm::vec3>(plane); float distance = planeReader.at<float>(plane + 12);
            float a = glm::dot(glm::mix(start, end, segment.begin), normal) - distance;
            float b = glm::dot(glm::mix(start, end, segment.end), normal) - distance;
            auto front = nodeReader.at<int32_t>(node + 4), back = nodeReader.at<int32_t>(node + 8);
            if (a >= 0 && b >= 0) { stack.push_back({front, segment.begin, segment.end}); }
            else if (a < 0 && b < 0) { stack.push_back({back, segment.begin, segment.end}); }
            else {
                float middle = glm::mix(segment.begin, segment.end, std::clamp(a / (a - b), 0.0f, 1.0f));
                stack.push_back({a >= 0 ? back : front, middle, segment.end});
                stack.push_back({a >= 0 ? front : back, segment.begin, middle});
            }
            continue;
        }
        size_t leaf = size_t(-int64_t(segment.node) - 1) * 32;
        auto first = leafReader.at<uint16_t>(leaf + 24), count = leafReader.at<uint16_t>(leaf + 26);
        for (uint32_t i = 0; i < count; ++i) {
            auto brush = referenceReader.at<uint16_t>((size_t(first) + i) * 2);
            require(brush < brushes.size() / 12, "Lighting trace references an invalid brush.");
            if (checkedBrushes[brush] == trace) { continue; } checkedBrushes[brush] = trace;
            size_t record = size_t(brush) * 12;
            if (!(brushReader.at<uint32_t>(record + 8) & 1)) { continue; }
            auto sideFirst = brushReader.at<uint32_t>(record), sideCount = brushReader.at<uint32_t>(record + 4);
            sideReader.slice(size_t(sideFirst) * 8, size_t(sideCount) * 8);
            float enter = 0, leave = 1; int enterTexture = -1;
            bool outside = false, rejected = false;
            for (uint32_t j = 0; j < sideCount; ++j) {
                size_t side = (size_t(sideFirst) + j) * 8;
                size_t plane = size_t(sideReader.at<uint16_t>(side)) * 20;
                auto normal = planeReader.at<glm::vec3>(plane); float distance = planeReader.at<float>(plane + 12);
                float a = glm::dot(start, normal) - distance, b = glm::dot(end, normal) - distance;
                outside |= a > 0;
                if (a > 0 && b > 0) { rejected = true; break; }
                if (a <= 0 && b <= 0) { continue; }
                float fraction = a / (a - b);
                if (a > b && fraction >= enter) { enter = fraction; enterTexture = sideReader.at<int16_t>(side + 2); }
                else if (a < b) { leave = std::min(leave, fraction); }
            }
            if (!rejected && enter <= leave && enter < nearest) {
                nearest = outside ? enter : 0;
                hitSky = enterTexture >= 0 && (textureReader.at<uint32_t>(size_t(enterTexture) * 72 + 64) & 6) != 0;
            }
        }
    }
    return sky ? nearest < 1 && hitSky : nearest >= 0.9999f;
}

PropLighting WorldLighting::sample(glm::vec3 position) const {
    struct Candidate { float strength; PropLight light; };
    std::vector<Candidate> candidates;
    for (const auto& light : lights) {
        if (light.type == 5 || light.type == 4 || (light.flags & 1)) { continue; }
        glm::vec3 direction = light.type == 3 ? -light.normal : light.origin - position;
        float distance = glm::length(direction);
        if (distance < 0.001f) { continue; } direction /= distance;
        if (light.type != 3 && light.radius > 0 && distance > light.radius) { continue; }
        float attenuation = 1;
        auto factors = light.attenuation;
        if (light.type == 0) { factors = {0, 0, 1}; }
        if (light.type != 3) { attenuation = 1 / std::max(glm::dot(factors, glm::vec3(1, distance, distance * distance)), 0.00001f); }
        if (light.type == 0) { attenuation *= std::max(0.0f, glm::dot(-direction, light.normal)); }
        if (light.type == 2) {
            float cone = glm::dot(-direction, light.normal);
            attenuation *= std::pow(std::clamp((cone - light.outer) / std::max(light.inner - light.outer, 0.00001f), 0.0f, 1.0f), light.exponent);
        }
        float strength = std::max({light.intensity.r, light.intensity.g, light.intensity.b}) * attenuation;
        if (strength < 0.0001f) { continue; }
        if (!visible(position + direction * 0.05f, light.type == 3 ? position + direction * 131072.0f : light.origin, light.type == 3)) { continue; }
        PropLight result;
        result.positionType = {light.type == 3 ? direction : light.origin, light.type == 3 ? 1.0f : light.type == 2 ? 2.0f : 0.0f};
        result.color = {light.intensity, light.inner}; result.attenuation = {factors, light.exponent}; result.directionCone = {light.normal, light.outer};
        if (light.type == 0) { result.color *= std::max(0.0f, glm::dot(-direction, light.normal)); }
        candidates.push_back({strength, result});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.strength > b.strength; });
    PropLighting result; result.count = static_cast<uint32_t>(std::min(candidates.size(), result.lights.size()));
    for (uint32_t i = 0; i < result.count; ++i) { result.lights[i] = candidates[i].light; }
    return result;
}
}
