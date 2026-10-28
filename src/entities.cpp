#include <scm/entities.hpp>
#include <algorithm>

namespace scm {
namespace {
uint32_t entityFlags(const bsppp::BSPEntityKeyValues& entity) {
    float value = parseFloat(entity["spawnflags"].getValue());
    require(value >= 0 && value <= 16777215 && std::floor(value) == value, "Invalid track spawn flags.");
    return static_cast<uint32_t>(value);
}
}

EntityPose initialTrackTrainPose(const Bsp& bsp, const bsppp::BSPEntityKeyValues& train, double captureTime) {
    require(captureTime == 0, "Tracktrains support the initial path pose only; a nonzero capture time requires gameplay simulation.");
    require(train["parentname"].getValue().empty(), "Parented tracktrains require runtime parent transforms.");
    using Entity = bsppp::BSPEntityKeyValues;
    std::map<std::string, const Entity*> nodes;
    for (const auto& entity : bsp.entities) {
        if (entity["classname"].getValue() != "path_track") { continue; }
        auto name = std::string(entity["targetname"].getValue());
        if (name.empty()) { continue; }
        require(nodes.emplace(name, &entity).second, std::format("Ambiguous path_track name '{}'.", name));
    }
    auto resolve = [&](std::string_view name) -> const Entity* {
        if (name.empty()) { return nullptr; }
        auto found = nodes.find(std::string(name));
        require(found != nodes.end(), std::format("Missing path_track '{}'.", name));
        require(found->second->operator[]("parentname").getValue().empty(), "Parented path tracks require runtime transforms.");
        return found->second;
    };
    const Entity* start = resolve(train["target"].getValue());
    require(start != nullptr, "Tracktrain has no initial path target.");
    auto previous = [&](const Entity* node) -> const Entity* {
        const Entity* result = nullptr;
        for (const auto& entity : bsp.entities) {
            if (entity["classname"].getValue() == "path_track" && entity["target"].getValue() == (*node)["targetname"].getValue() && entity["targetname"].getValue() != (*node)["altpath"].getValue()) { result = &entity; }
        }
        return result;
    };
    auto neighbor = [&](const Entity* node, bool forward) -> const Entity* {
        auto flags = entityFlags(*node);
        if ((flags & 0x8000) && !(*node)["altpath"].getValue().empty() && bool(flags & 4) != forward) { return resolve((*node)["altpath"].getValue()); }
        return forward ? resolve((*node)["target"].getValue()) : previous(node);
    };
    EntityPose pose{parseVector((*start)["origin"].getValue()), parseVector(train["angles"].getValue())};
    auto look = pose.origin;
    float distance = parseFloat(train["wheels"].getValue());
    bool forward = distance >= 0; distance = std::abs(distance);
    const Entity* current = start;
    unsigned steps = 0;
    while (distance > 0) {
        require(++steps <= 65536, "Track lookahead contains a zero-length cycle or exceeds the traversal limit.");
        auto next = neighbor(current, forward);
        if (!next) {
            if (auto behind = neighbor(current, !forward)) {
                auto direction = parseVector((*current)["origin"].getValue()) - parseVector((*behind)["origin"].getValue());
                if (glm::length(direction) > 0) { look += glm::normalize(direction) * distance; }
            }
            break;
        }
        auto position = parseVector((*next)["origin"].getValue());
        auto delta = position - look; float length = glm::length(delta);
        if (length > distance) { look += delta * (distance / length); break; }
        look = position; distance -= length; current = next;
    }
    auto flags = entityFlags(train);
    if (!(flags & 16)) {
        auto direction = look - pose.origin;
        float horizontal = glm::length(glm::vec2(direction));
        pose.angles = {horizontal > 0 ? glm::degrees(std::atan2(-direction.z, horizontal)) : direction.z > 0 ? -90.0f : 90.0f, horizontal > 0 ? glm::degrees(std::atan2(direction.y, direction.x)) : 0.0f, 0};
        if (flags & 1) { pose.angles.x = 0; }
    }
    pose.origin.z += parseFloat(train["height"].getValue());
    return pose;
}
EntityPose initialPathTrainPose(const Bsp& bsp, const bsppp::BSPEntityKeyValues& train, glm::vec3 center, double captureTime) {
    require(captureTime == 0, "Path trains support the initial path pose only; a nonzero capture time requires gameplay simulation.");
    require(train["parentname"].getValue().empty(), "Parented path trains require runtime parent transforms.");
    auto target = train["target"].getValue();
    require(!target.empty(), "Path train has no initial path target.");
    const bsppp::BSPEntityKeyValues* found = nullptr;
    for (const auto& entity : bsp.entities) {
        if (entity["targetname"].getValue() != target) { continue; }
        require(!found, std::format("Ambiguous path train target '{}'.", target)); found = &entity;
    }
    require(found && (*found)["classname"].getValue() == "path_corner", std::format("Missing path_corner '{}' for path train.", target));
    require((*found)["parentname"].getValue().empty(), "Parented path corners require runtime transforms.");
    return {parseVector((*found)["origin"].getValue()) - center, parseVector(train["angles"].getValue())};
}
}
