#include <scm/scene.hpp>
#include <scm/cache.hpp>
#include <scm/lighting.hpp>
#include <vtfpp/VTF.h>
#include <mdlpp/mdlpp.h>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>
#include <sstream>
#include <tuple>

namespace scm {
namespace {
template<class T> std::vector<T> records(const Bytes& bytes, std::string_view label) {
    require(bytes.size() % sizeof(T) == 0, std::format("Unsupported {} record layout.", label));
    std::vector<T> result(bytes.size() / sizeof(T));
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}
template<class V> glm::vec3 vec3(const V& value) { return {value[0], value[1], value[2]}; }
template<class V> glm::vec4 vec4(const V& value) { return {value[0], value[1], value[2], value[3]}; }
glm::vec3 rgbExp(const Reader& reader, size_t offset) {
    float scale = std::ldexp(1.0f / 255.0f, reader.at<int8_t>(offset + 3));
    return glm::vec3(reader.at<uint8_t>(offset), reader.at<uint8_t>(offset + 1), reader.at<uint8_t>(offset + 2)) * scale;
}
glm::mat4 transform(glm::vec3 origin, glm::vec3 angles) {
    auto matrix = glm::translate(glm::mat4(1), origin);
    matrix = glm::rotate(matrix, glm::radians(angles.y), glm::vec3(0, 0, 1));
    matrix = glm::rotate(matrix, glm::radians(angles.x), glm::vec3(0, 1, 0));
    return glm::rotate(matrix, glm::radians(angles.z), glm::vec3(1, 0, 0));
}
glm::vec2 transformedUV(glm::vec2 uv, std::string_view description) {
    if (description.empty()) { return uv; }
    std::istringstream stream{std::string(description)};
    std::string key;
    glm::vec2 center(0.5f), scale(1), translation(0);
    float angle = 0;
    while (stream >> key) {
        if (key == "center") { stream >> center.x >> center.y; }
        else if (key == "scale") { stream >> scale.x >> scale.y; }
        else if (key == "rotate") { stream >> angle; }
        else if (key == "translate") { stream >> translation.x >> translation.y; }
        else { throw Error(std::format("Unsupported texture transform: {}", description)); }
    }
    uv = (uv - center) * scale;
    float radians = glm::radians(angle);
    return glm::vec2(std::cos(radians) * uv.x - std::sin(radians) * uv.y, std::sin(radians) * uv.x + std::cos(radians) * uv.y) + center + translation;
}

class Builder {
public:
    Builder(const Bsp& input, Assets& resolver, const SceneOptions& settings) : bsp(input), assets(resolver), options(settings) {
        scene.hdr = options.hdr;
        scene.probes = bsp.probes;
        scene.images.push_back({"white", 1, 1, false, false, {glm::vec4(1)}});
        scene.images.push_back({"flat normal", 1, 1, false, false, {glm::vec4(0.5f, 0.5f, 1, 1)}});
        scene.diagnostics = {{"unsupported", Json::array()}, {"warnings", Json::array()}};
        lightStyles = initialLightStyles(bsp);
    }
    uint32_t texture(std::string name, bool srgb, std::string_view requester) {
        if (name.empty()) { return 0; }
        name = assetPath(name);
        if (!name.starts_with("materials/")) { name = "materials/" + name; }
        if (!name.ends_with(".vtf")) { name += ".vtf"; }
        std::string key = name + (srgb ? ":srgb" : ":linear");
        if (textureIds.contains(key)) { return textureIds.at(key); }
        auto asset = assets.get(name, requester);
        validateVtf(asset->bytes, name);
        vtfpp::VTF vtf(asset->bytes, false, options.hdr);
        require(static_cast<bool>(vtf), std::format("Invalid texture {}.", name));
        require(vtf.getFrameCount() == 1, std::format("Animated VTF is not supported yet: {}", name));
        require(vtf.getFaceCount() == 1 && vtf.getDepth() == 1, std::format("Expected a 2D texture: {}", name));
        bool floating = vtf.getFormat() == vtfpp::ImageFormat::RGBA16161616F || vtf.getFormat() == vtfpp::ImageFormat::RGB323232F || vtf.getFormat() == vtfpp::ImageFormat::RGBA32323232F;
        Image image;
        image.name = name;
        image.width = vtf.getWidth(); image.height = vtf.getHeight(); image.srgb = srgb && !floating;
        image.mipCount = vtf.getMipCount();
        image.clamp = (vtf.getFlags() & (vtfpp::VTF::FLAG_V0_CLAMP_S | vtfpp::VTF::FLAG_V0_CLAMP_T)) != 0;
        image.pixels.clear();
        for (uint32_t mip = 0; mip < image.mipCount; ++mip) {
            size_t count = size_t(std::max(1u, image.width >> mip)) * std::max(1u, image.height >> mip);
            require((image.pixels.size() + count) * sizeof(glm::vec4) <= (1ull << 30), std::format("Decoded texture '{}' exceeds the 1 GiB limit.", name));
            auto pixels = vtf.getImageDataAs(vtfpp::ImageFormat::RGBA32323232F, static_cast<uint8_t>(mip));
            require(pixels.size() == count * sizeof(glm::vec4), std::format("Failed to decode mip {} of {}.", mip, name));
            auto offset = image.pixels.size(); image.pixels.resize(offset + count);
            std::memcpy(image.pixels.data() + offset, pixels.data(), pixels.size());
        }
        uint32_t index = static_cast<uint32_t>(scene.images.size());
        scene.images.push_back(std::move(image));
        textureIds.emplace(key, index);
        return index;
    }
    uint32_t material(std::string name) {
        name = lower(std::move(name));
        if (materialIds.contains(name)) { return materialIds.at(name); }
        Material material;
        material.name = name;
        auto& definition = material.definition = readMaterial(assets, name, options.hdr);
        auto shader = definition.shader;
        bool painted = shader == "g_sky";
        require(shader == "lightmappedgeneric" || shader == "unlitgeneric" || shader == "worldvertextransition" || shader == "vertexlitgeneric" || shader == "sky" || painted, std::format("Unsupported shader '{}' in '{}'. No BSP was written.", shader, name));
        require(definition.proxies.empty() || (painted && definition.proxies.size() == 1 && lower(definition.proxies.front()) == "skypaint"), std::format("Material proxies are not implemented for '{}'. No BSP was written.", name));
        if (shader == "unlitgeneric" || shader == "sky" || painted) { material.flags |= Unlit; }
        if (shader == "worldvertextransition") { material.flags |= Blend; }
        if (shader == "vertexlitgeneric") { material.flags |= VertexLit; }
        if (painted) { material.flags |= Painted; }
        for (auto parameter : {"$bumpmap2", "$blendmodulatetexture", "$lightwarptexture", "$selfillummask", "$selfillumfresnel", "$seamless_scale", "$basetexturetransform2", "$detailtexturetransform"}) {
            require(definition.value(parameter).empty() || definition.value(parameter) == "0", std::format("Material parameter '{}' is not implemented for '{}'.", parameter, name));
        }
        require(definition.value("$bumptransform").empty() || definition.value("$bumptransform") == definition.value("$basetexturetransform"), std::format("Independent bump texture coordinates are not implemented for '{}'.", name));
        auto environment = definition.value("$envmap");
        require(environment.empty() || lower(environment) == "env_cubemap" || assetPath(environment).starts_with("maps/"), std::format("Static environment textures are not implemented for '{}'. Only probe-driven reflection feedback can be suppressed.", name));
        auto base = definition.value(options.hdr ? "$hdrbasetexture" : "$basetexture");
        if (base.empty()) { base = definition.value("$basetexture"); }
        material.base = texture(base, true, name);
        material.second = texture(definition.value("$basetexture2"), true, name);
        if (!definition.value("$bumpmap").empty()) { material.normal = texture(definition.value("$bumpmap"), false, name); material.flags |= NormalMap; }
        if (!definition.value("$detail").empty()) { material.detail = texture(definition.value("$detail"), false, name); material.flags |= Detail; }
        if (definition.flag("$alphatest")) { material.flags |= AlphaTest; }
        if (definition.flag("$selfillum")) { material.flags |= SelfIllum; }
        material.translucent = definition.flag("$translucent"); material.additive = definition.flag("$additive"); material.noCull = definition.flag("$nocull");
        material.noFog = definition.flag("$nofog");
        if (definition.flag("$vertexalpha")) { material.flags |= VertexAlpha; material.translucent = true; }
        material.alphaReference = parseFloat(definition.value("$alphatestreference"), 0.5f);
        material.detailScale = parseFloat(definition.value("$detailscale"), 4);
        material.detailFactor = parseFloat(definition.value("$detailblendfactor"), 1);
        material.detailMode = static_cast<int>(parseFloat(definition.value("$detailblendmode"), 0));
        require(!(material.flags & Detail) || material.detailMode == 0, std::format("Detail blend mode {} is not implemented for {}.", material.detailMode, name));
        material.tint = glm::vec4(parseVector(definition.value("$color", definition.value("$color2")), glm::vec3(1)), parseFloat(definition.value("$alpha"), 1));
        if (definition.flag("$ssbump")) { require(material.flags & NormalMap, "SSBump requires a bump texture."); material.flags |= SSBump; }
        require(definition.value("$selfillumtint").empty() || parseVector(definition.value("$selfillumtint")) == glm::vec3(1), std::format("Self-illumination tint is not implemented for '{}'.", name));
        if (definition.flag("$phong")) {
            require(shader == "vertexlitgeneric", std::format("Phong is supported only for VertexLitGeneric: {}", name));
            for (auto key : {"$phongwarptexture", "$rimlight", "$phongexponenttexturemask"}) {
                require(definition.value(key).empty() || definition.value(key) == "0", std::format("Unsupported Phong control '{}' in '{}'.", key, name));
            }
            material.flags |= Phong;
            if (!definition.flag("$phongdisablehalflambert")) { material.flags |= HalfLambert; }
            material.phongExponent = parseFloat(definition.value("$phongexponent"), 5);
            material.phongBoost = parseFloat(definition.value("$phongboost"), 1);
            material.phongExponentFactor = parseFloat(definition.value("$phongexponentfactor"), 149);
            material.phongFresnel = parseVector(definition.value("$phongfresnelranges"), {0, 0.5f, 1});
            material.phongTint = parseVector(definition.value("$phongtint"), glm::vec3(1));
            require(material.phongExponent > 0 && material.phongBoost >= 0 && material.phongExponentFactor >= 0, std::format("Invalid Phong parameters in '{}'.", name));
            if (!definition.value("$phongexponenttexture").empty() && definition.value("$phongexponent").empty()) {
                material.second = texture(definition.value("$phongexponenttexture"), false, name); material.flags |= PhongExponentTexture;
            }
            if (definition.flag("$basemapalphaphongmask")) { material.flags |= PhongBaseAlpha; }
            if (definition.flag("$invertphongmask")) { material.flags |= PhongInvertMask; }
            if (definition.flag("$phongalbedotint")) { material.flags |= PhongAlbedoTint; }
        }
        if (definition.flag("$halflambert")) { material.flags |= HalfLambert; }
        if (definition.flag("$blendtintbybasealpha")) {
            material.flags |= BlendTintByAlpha;
            material.blendTintOverBase = parseFloat(definition.value("$blendtintcoloroverbase"), 0);
            require(material.blendTintOverBase >= 0 && material.blendTintOverBase <= 1, "Invalid material tint replacement factor.");
        }
        uint32_t index = static_cast<uint32_t>(scene.materials.size());
        scene.materials.push_back(std::move(material)); materialIds.emplace(name, index);
        return index;
    }
    int leafAt(glm::vec3 point) const {
        Reader nodes(nodesData, "nodes"), planes(planeData, "planes");
        int index = 0;
        for (size_t steps = 0; steps <= nodesData.size() / 32; ++steps) {
            if (index < 0) { return -1 - index; }
            size_t start = size_t(index) * 32;
            auto plane = nodes.at<uint32_t>(start);
            auto normal = planes.at<glm::vec3>(size_t(plane) * 20);
            auto distance = planes.at<float>(size_t(plane) * 20 + 12);
            index = nodes.at<int32_t>(start + (glm::dot(normal, point) >= distance ? 4 : 8));
        }
        throw Error("Cycle in BSP nodes.");
    }
    std::set<uint32_t> visibleFaces(glm::vec3 point) const {
        Reader leaves(leafData, "leaves"), vis(visData, "PVS"), faceRefs(leafFaceData, "leaf faces");
        std::set<uint32_t> result;
        int leaf = leafAt(point);
        int cluster = leaves.at<int16_t>(size_t(leaf) * 32 + 4);
        std::vector<uint8_t> visible;
        if (cluster >= 0 && !visData.empty()) {
            int count = vis.at<int32_t>(0);
            require(count >= 0 && cluster < count, "Invalid PVS cluster.");
            int offset = vis.at<int32_t>(4 + size_t(cluster) * 8);
            size_t desired = (size_t(count) + 7) / 8;
            while (visible.size() < desired) {
                uint8_t value = vis.at<uint8_t>(offset++);
                if (value) { visible.push_back(value); }
                else {
                    uint8_t run = vis.at<uint8_t>(offset++);
                    require(run > 0 && visible.size() + run <= desired, "Invalid PVS run.");
                    visible.insert(visible.end(), run, 0);
                }
            }
        }
        for (size_t i = 0; i < leafData.size() / 32; ++i) {
            int candidate = leaves.at<int16_t>(i * 32 + 4);
            if (!visible.empty() && (candidate < 0 || size_t(candidate / 8) >= visible.size() || !(visible[candidate / 8] & (1 << (candidate % 8))))) { continue; }
            auto first = leaves.at<uint16_t>(i * 32 + 20), count = leaves.at<uint16_t>(i * 32 + 22);
            for (size_t j = 0; j < count; ++j) { result.insert(faceRefs.at<uint16_t>((first + j) * 2)); }
        }
        return result;
    }
    struct LightPlacement { std::array<uint32_t, 4> images{}; glm::vec2 offset{}, size{1}; };
    LightPlacement lightmap(const bsppp::BSPFace_v1& face, bool bumped) {
        if (face.lightOffset < 0) { return {}; }
        size_t styleCount = 0;
        while (styleCount < 4 && face.styles[styleCount] != 255) {
            require(std::isfinite(lightStyles[face.styles[styleCount]]), "A face references an unresolved initial light style."); ++styleCount;
        }
        require(styleCount > 0, "A lit face has no light styles.");
        uint32_t width = face.lightmapTextureSizeInLuxels[0] + 1, height = face.lightmapTextureSizeInLuxels[1] + 1;
        require(width > 0 && height > 0 && width + 2 <= 1024 && height + 2 <= 1024, "Unsupported lightmap dimensions.");
        if (pageX + width + 2 > 1024) { pageX = 0; pageY += pageRow; pageRow = 0; }
        if (pageIds[0] == 0 || pageY + height + 2 > 1024) {
            pageX = pageY = pageRow = 0;
            for (unsigned i = 0; i < 4; ++i) {
                pageIds[i] = static_cast<uint32_t>(scene.images.size());
                scene.images.push_back({std::format("lightmap-{}-{}", pageIds[0], i), 1024, 1024, false, true, std::vector<glm::vec4>(1024 * 1024, glm::vec4(0, 0, 0, 1))});
            }
        }
        LightPlacement result{pageIds, glm::vec2(pageX + 1, pageY + 1), glm::vec2(width, height)};
        Reader lighting(lightData, "lightmaps");
        for (int y = -1; y <= int(height); ++y) {
            for (int x = -1; x <= int(width); ++x) {
                auto pixel = uint32_t(std::clamp(y, 0, int(height) - 1)) * width + uint32_t(std::clamp(x, 0, int(width) - 1));
                std::array<glm::vec3, 4> samples{};
                for (unsigned plane = 0; plane < 4; ++plane) {
                    unsigned sourcePlane = bumped ? plane : 0;
                    for (size_t style = 0; style < styleCount; ++style) {
                        auto sample = ((style * (bumped ? 4 : 1) + sourcePlane) * width * height + pixel) * 4;
                        samples[plane] += rgbExp(lighting, face.lightOffset + sample) * lightStyles[face.styles[style]];
                    }
                }
                auto radiance = lightmapRadiance(samples, bumped, options.hdr);
                for (unsigned plane = 0; plane < 4; ++plane) { scene.images[pageIds[plane]].pixels[(pageY + 1 + y) * 1024 + pageX + 1 + x] = glm::vec4(radiance[plane], 1); }
            }
        }
        pageX += width + 2; pageRow = std::max(pageRow, height + 2);
        return result;
    }
    Scene build();
    void addProps();
    void addSky();
    Scene scene;
    const Bsp& bsp;
    Assets& assets;
    SceneOptions options;
    Bytes nodesData, planeData, leafData, leafFaceData, visData, lightData;
    std::set<uint32_t> skyFaces;
    int skyArea = -1;
    std::map<std::string, uint32_t> textureIds, materialIds;
    std::array<uint32_t, 4> pageIds{};
    std::array<float, 256> lightStyles{};
    uint32_t pageX = 0, pageY = 0, pageRow = 0;
};

Scene Builder::build() {
    auto unsupported = [&](std::string message) { require(options.experimental, message + " Use --experimental to explicitly omit this feature."); scene.diagnostics["warnings"].push_back(std::move(message)); };
    if (bsp.lumps[45].length) { unsupported("Compiled overlays are not rendered."); }
    for (const auto& entity : bsp.entities) {
        auto type = std::string(entity["classname"].getValue()), model = std::string(entity["model"].getValue());
        bool runtimeModel = !model.empty() && model.front() != '*' && type != "worldspawn";
        bool runtimeEffect = type == "move_rope" || type == "keyframe_rope" || type == "env_sprite" || type == "env_beam" || type == "env_smokestack" || type == "env_fire" || type == "info_particle_system" || type == "func_smokevolume";
        if (runtimeModel || runtimeEffect) { unsupported(std::format("Runtime-dependent entity '{}' ({}) is not rendered.", type, model.empty() ? std::string(entity["origin"].getValue()) : model)); }
    }
    require(bsp.lumps[7].version == 1 && bsp.lumps[10].version == 1, "Only face v1 and leaf v1 layouts are supported.");
    if (options.hdr && bsp.lumps[58].length) { require(bsp.lumps[58].version == 1, "Unsupported HDR face layout."); }
    for (auto [id, stride] : std::array<std::pair<int, size_t>, 17>{{{1,20},{2,32},{3,12},{5,32},{6,72},{12,4},{13,4},{14,48},{16,2},{26,176},{33,20},{37,10},{38,12},{39,2},{43,1},{44,4},{48,2}}}) {
        require(bsp.lumps[id].version == 0 && bsp.lump(id).size() % stride == 0, std::format("Unsupported record version or stride in lump {}.", id));
    }
    require(bsp.lump(10).size() % 32 == 0, "Leaf data has a partial record.");
    auto faceData = bsp.lump(options.hdr && bsp.lumps[58].length ? 58 : 7);
    auto faces = records<bsppp::BSPFace_v1>(faceData, "faces");
    auto points = bsp.source->getLumpData<bsppp::BSPLump::VERTEXES>();
    auto edges = bsp.source->getLumpData<bsppp::BSPLump::EDGES>();
    auto surfEdges = bsp.source->getLumpData<bsppp::BSPLump::SURFEDGES>();
    auto texInfo = bsp.source->getLumpData<bsppp::BSPLump::TEXINFO>();
    auto texData = bsp.source->getLumpData<bsppp::BSPLump::TEXDATA>();
    auto brushModels = bsp.source->getLumpData<bsppp::BSPLump::MODELS>();
    require(!brushModels.empty(), "The BSP has no world model.");
    struct BrushState { glm::mat4 matrix{1}; glm::vec4 color{1}; bool hidden = false; };
    std::vector<BrushState> brushStates(brushModels.size());
    std::vector<uint32_t> faceModels(faces.size(), 0);
    for (size_t m = 1; m < brushModels.size(); ++m) {
        auto found = std::find_if(bsp.entities.begin(), bsp.entities.end(), [&](const auto& entity) { return entity["model"].getValue() == "*" + std::to_string(m); });
        require(found != bsp.entities.end(), "A brush model has no corresponding entity.");
        auto type = std::string((*found)["classname"].getValue());
        auto& state = brushStates[m];
        state.hidden = type.starts_with("trigger_") || type == "func_vehicleclip" || type == "func_clip_vphysics" || type == "func_occluder" || type == "func_areaportal" || type == "func_areaportalwindow" || parseFloat((*found)["StartDisabled"].getValue()) != 0;
        state.hidden = state.hidden || type == "func_smokevolume";
        bool door = type == "func_door" || type == "func_door_rotating";
        require(state.hidden || door || type == "func_brush" || type == "func_illusionary" || type == "func_wall" || type == "func_button" || type == "func_reflective_glass" || type == "func_breakable", std::format("Unsupported initial brush-entity behavior: {}", type));
        require(state.hidden || (parseFloat((*found)["rendermode"].getValue()) == 0 && parseFloat((*found)["renderfx"].getValue()) == 0), "Brush-entity render effects are not implemented.");
        auto origin = parseVector((*found)["origin"].getValue()), angles = parseVector((*found)["angles"].getValue());
        const auto& model = brushModels[m];
        if (door) {
            float flagsValue = parseFloat((*found)["spawnflags"].getValue()), spawn = parseFloat((*found)["spawnpos"].getValue());
            require(flagsValue >= 0 && flagsValue <= 131071 && std::floor(flagsValue) == flagsValue && (spawn == 0 || spawn == 1), "Invalid initial door state.");
            auto flags = static_cast<uint32_t>(flagsValue);
            if ((flags & 1) || spawn == 1) {
                require((*found)["parentname"].getValue().empty(), "A parented door starting open requires runtime parent transforms.");
                if (type == "func_door_rotating") {
                    unsigned axis = (flags & 64) ? 2 : (flags & 128) ? 0 : 1;
                    angles[axis] += parseFloat((*found)["distance"].getValue()) * ((flags & 2) ? -1.0f : 1.0f);
                } else {
                    auto direction = glm::vec3(transform(glm::vec3(0), parseVector((*found)["movedir"].getValue())) * glm::vec4(1,0,0,0));
                    auto size = vec3(model.max) - vec3(model.min);
                    origin += direction * (glm::dot(glm::abs(direction), size) - parseFloat((*found)["lip"].getValue()));
                }
            }
        }
        state.matrix = transform(origin, angles);
        state.color = glm::vec4(parseVector((*found)["rendercolor"].getValue(), glm::vec3(255)) / 255.0f, parseFloat((*found)["renderamt"].getValue(), 255) / 255.0f);
        require(model.firstFace >= 0 && model.numFaces >= 0 && size_t(model.firstFace) + model.numFaces <= faces.size(), "Brush model face range is invalid.");
        for (int i = 0; i < model.numFaces; ++i) { faceModels[model.firstFace + i] = static_cast<uint32_t>(m); }
    }
    nodesData = bsp.lump(5); planeData = bsp.lump(1); leafData = bsp.lump(10); leafFaceData = bsp.lump(16); visData = bsp.lump(4);
    lightData = bsp.lump(options.hdr ? 53 : 8);
    require(!lightData.empty(), "Requested compiled lighting is missing.");
    auto stringsData = bsp.lump(43), stringOffsets = bsp.lump(44), displacements = bsp.lump(26), dispVertices = bsp.lump(33);
    auto primitiveData = bsp.lump(37), primitiveVertexData = bsp.lump(38), primitiveIndexData = bsp.lump(39);
    Reader primitives(primitiveData, "primitives"), primitiveVertices(primitiveVertexData, "primitive vertices"), primitiveIndices(primitiveIndexData, "primitive indices");
    require(primitiveData.size() % 10 == 0 && primitiveVertexData.size() % 12 == 0 && primitiveIndexData.size() % 2 == 0, "Unsupported compiled primitive layout.");
    Reader strings(stringsData, "material names"), offsets(stringOffsets, "material string offsets"), planes(planeData, "planes"), disp(displacements, "displacements"), dispVerts(dispVertices, "displacement vertices");
    for (const auto& entity : bsp.entities) {
        if (entity["classname"].getValue() == "sky_camera") {
            require(scene.skyScale == 0, "Multiple 3D sky cameras are not supported.");
            require(!visData.empty(), "A 3D skybox requires compiled visibility data for scene classification.");
            scene.skyOrigin = parseVector(entity["origin"].getValue());
            scene.skyScale = parseFloat(entity["scale"].getValue(), 16);
            require(scene.skyScale > 0, "3D sky cameras require a positive scale.");
            skyFaces = visibleFaces(scene.skyOrigin);
            skyArea = Reader(leafData, "sky leaf").at<uint16_t>(size_t(leafAt(scene.skyOrigin)) * 32 + 6) & 511;
            if (parseFloat(entity["fogenable"].getValue()) != 0) {
                scene.skyFogColor = parseVector(entity["fogcolor"].getValue()) / 255.0f;
                scene.skyFogStart = parseFloat(entity["fogstart"].getValue()) / scene.skyScale;
                scene.skyFogEnd = parseFloat(entity["fogend"].getValue()) / scene.skyScale;
            }
        } else if (entity["classname"].getValue() == "env_fog_controller" && parseFloat(entity["fogenable"].getValue()) != 0) {
            require(scene.fogEnd == 0, "Multiple enabled fog controllers need runtime state selection.");
            scene.fogColor = parseVector(entity["fogcolor"].getValue()) / 255.0f;
            scene.fogStart = parseFloat(entity["fogstart"].getValue()); scene.fogEnd = parseFloat(entity["fogend"].getValue());
        }
    }
    std::vector<std::set<uint32_t>> probeFaces;
    for (const auto& probe : bsp.probes) { probeFaces.push_back(visibleFaces(glm::vec3(probe.origin))); }
    std::set<uint32_t> referencedFaces;
    Reader referenced(leafFaceData, "leaf face references");
    for (size_t i = 0; i < leafFaceData.size(); i += 2) { referencedFaces.insert(referenced.at<uint16_t>(i)); }
    std::map<std::vector<uint64_t>, uint32_t> visibilityIds;
    std::vector<std::vector<uint64_t>> visibilityMasks;
    std::map<std::tuple<uint32_t, uint32_t, bool, uint32_t>, std::vector<uint32_t>> batches;
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto& face = faces[faceIndex];
        const auto& brush = brushStates[faceModels[faceIndex]];
        if (brush.hidden) { continue; }
        require(face.texInfo >= 0 && size_t(face.texInfo) < texInfo.size(), "Face references invalid texture information.");
        const auto& info = texInfo.at(face.texInfo);
        if (info.flags & (0x2 | 0x4 | 0x80 | 0x200)) { continue; }
        bool inSkyWorld = skyFaces.contains(static_cast<uint32_t>(faceIndex));
        if (face.dispInfo >= 0 && skyArea >= 0) {
            auto point = disp.at<glm::vec3>(size_t(face.dispInfo) * 176);
            point += planes.at<glm::vec3>(size_t(face.planeNum) * 20) * (face.side ? -64.0f : 64.0f);
            auto area = Reader(leafData, "displacement leaf").at<uint16_t>(size_t(leafAt(point)) * 32 + 6) & 511;
            inSkyWorld = area == skyArea;
        }
        std::vector<uint64_t> visible((bsp.probes.size() + 63) / 64);
        bool anyVisible = false;
        for (size_t p = 0; p < probeFaces.size(); ++p) {
            if (faceModels[faceIndex] != 0 || inSkyWorld || !referencedFaces.contains(static_cast<uint32_t>(faceIndex)) || probeFaces[p].contains(static_cast<uint32_t>(faceIndex))) { visible[p / 64] |= uint64_t(1) << (p % 64); anyVisible = true; }
        }
        if (!anyVisible) { continue; }
        auto [foundVisibility, insertedVisibility] = visibilityIds.emplace(visible, static_cast<uint32_t>(visibilityMasks.size()));
        if (insertedVisibility) { visibilityMasks.push_back(visible); }
        const auto& tex = texData.at(info.textureData);
        uint32_t nameOffset = offsets.at<uint32_t>(size_t(tex.nameStringTableID) * 4);
        auto name = strings.string(nameOffset, stringsData.size() - nameOffset);
        auto materialIndex = material(name);
        const auto& mat = scene.materials[materialIndex];
        bool bumped = (info.flags & 0x800) != 0;
        require(!(mat.flags & SSBump) || bumped, std::format("SSBump material '{}' has no directional lightmaps.", name));
        auto placement = lightmap(face, bumped);
        std::vector<glm::vec3> polygon;
        for (int i = 0; i < face.numEdges; ++i) {
            int edgeIndex = surfEdges.at(size_t(face.firstEdge) + i).surfEdge;
            const auto& edge = edges.at(static_cast<size_t>(std::abs(int64_t(edgeIndex))));
            polygon.push_back(vec3(points.at(edgeIndex >= 0 ? edge.v0 : edge.v1).position));
        }
        require(polygon.size() >= 3, "Face has fewer than three vertices.");
        auto normal = planes.at<glm::vec3>(size_t(face.planeNum) * 20) * (face.side ? -1.0f : 1.0f);
        auto makeVertex = [&](glm::vec3 position, glm::vec3 uvPosition, float alpha) {
            Vertex vertex;
            vertex.position = glm::vec3(brush.matrix * glm::vec4(position, 1)); vertex.normal = glm::mat3(brush.matrix) * normal;
            auto tangent = glm::normalize(glm::vec3(vec4(info.textureVector1)));
            vertex.tangent = glm::vec4(glm::mat3(brush.matrix) * tangent, glm::dot(glm::cross(normal, tangent), glm::vec3(vec4(info.textureVector2))) < 0 ? -1 : 1);
            glm::vec4 homogeneous(uvPosition, 1);
            vertex.uv = {glm::dot(homogeneous, vec4(info.textureVector1)) / tex.width, glm::dot(homogeneous, vec4(info.textureVector2)) / tex.height};
            vertex.uv = transformedUV(vertex.uv, mat.definition.value("$basetexturetransform"));
            auto luxel = glm::vec2(glm::dot(homogeneous, vec4(info.lightmapVector1)), glm::dot(homogeneous, vec4(info.lightmapVector2)));
            vertex.lightUV = (luxel - glm::vec2(face.lightmapTextureMinsInLuxels[0], face.lightmapTextureMinsInLuxels[1]) + placement.offset + 0.5f) / 1024.0f;
            vertex.blend = alpha;
            vertex.color = brush.color;
            if ((mat.flags & VertexAlpha) && face.dispInfo >= 0) { vertex.color.a *= alpha; }
            return vertex;
        };
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        if (face.dispInfo >= 0) {
            require(polygon.size() == 4 && displacements.size() % 176 == 0, "Unsupported displacement base or layout.");
            size_t start = size_t(face.dispInfo) * 176;
            glm::vec3 anchor = disp.at<glm::vec3>(start);
            int firstVertex = disp.at<int32_t>(start + 12), power = disp.at<int32_t>(start + 20);
            require(power >= 2 && power <= 4 && firstVertex >= 0, "Unsupported displacement power.");
            auto closest = std::min_element(polygon.begin(), polygon.end(), [&](auto a, auto b) { return glm::dot(a - anchor, a - anchor) < glm::dot(b - anchor, b - anchor); });
            std::rotate(polygon.begin(), closest, polygon.end());
            uint32_t cells = 1u << power, side = cells + 1;
            for (uint32_t y = 0; y < side; ++y) {
                for (uint32_t x = 0; x < side; ++x) {
                    float u = float(x) / cells, v = float(y) / cells;
                    auto base = glm::mix(glm::mix(polygon[0], polygon[1], u), glm::mix(polygon[3], polygon[2], u), v);
                    size_t data = (size_t(firstVertex) + y * side + x) * 20;
                    auto position = base + dispVerts.at<glm::vec3>(data) * dispVerts.at<float>(data + 12);
                    vertices.push_back(makeVertex(position, base, dispVerts.at<float>(data + 16) / 255.0f));
                }
            }
            for (uint32_t y = 0; y < cells; ++y) { for (uint32_t x = 0; x < cells; ++x) {
                uint32_t a = y * side + x, b = a + 1, c = a + side, d = c + 1;
                if ((x + y) % 2) { indices.insert(indices.end(), {a, b, c, b, d, c}); }
                else { indices.insert(indices.end(), {a, b, d, a, d, c}); }
            } }
            for (auto& vertex : vertices) { vertex.normal = glm::vec3(0); }
            for (size_t i = 0; i < indices.size(); i += 3) {
                auto n = glm::cross(vertices[indices[i + 1]].position - vertices[indices[i]].position, vertices[indices[i + 2]].position - vertices[indices[i]].position);
                for (unsigned j = 0; j < 3; ++j) { vertices[indices[i + j]].normal += n; }
            }
            for (auto& vertex : vertices) { vertex.normal = glm::normalize(vertex.normal); }
        } else {
            for (auto position : polygon) { vertices.push_back(makeVertex(position, position, 0)); }
            if ((face.numPrims & 0x7fff) == 0) {
                for (uint32_t i = 1; i + 1 < polygon.size(); ++i) { indices.insert(indices.end(), {0, i, i + 1}); }
            } else {
                for (unsigned p = 0; p < (face.numPrims & 0x7fff); ++p) {
                    size_t record = (size_t(face.firstPrimID) + p) * 10;
                    auto type = primitives.at<uint8_t>(record);
                    auto first = primitives.at<uint16_t>(record + 2), count = primitives.at<uint16_t>(record + 4);
                    auto firstVertex = primitives.at<uint16_t>(record + 6), vertexCount = primitives.at<uint16_t>(record + 8);
                    uint32_t base = vertexCount ? static_cast<uint32_t>(vertices.size()) : 0;
                    for (unsigned v = 0; v < vertexCount; ++v) { auto position = primitiveVertices.at<glm::vec3>((size_t(firstVertex) + v) * 12); vertices.push_back(makeVertex(position, position, 0)); }
                    auto vertexIndex = [&](unsigned i) { auto value = base + primitiveIndices.at<uint16_t>((size_t(first) + i) * 2); require(value < vertices.size(), "Primitive index exceeds its face vertices."); return value; };
                    require(type <= 1 && count >= 3, "Unsupported primitive topology.");
                    if (type == 0) { require(count % 3 == 0, "Incomplete primitive triangle list."); for (unsigned i = 0; i < count; ++i) { indices.push_back(vertexIndex(i)); } }
                    else { for (unsigned i = 0; i + 2 < count; ++i) { indices.insert(indices.end(), {vertexIndex(i + (i & 1)), vertexIndex(i + 1 - (i & 1)), vertexIndex(i + 2)}); } }
                }
            }
        }
        auto& batch = batches[{materialIndex, placement.images[0], inSkyWorld, foundVisibility->second}];
        uint32_t base = static_cast<uint32_t>(scene.vertices.size());
        for (auto index : indices) { batch.push_back(base + index); }
        scene.vertices.insert(scene.vertices.end(), vertices.begin(), vertices.end());
    }
    for (auto& [key, indices] : batches) {
        auto [materialIndex, page, sky, visibilityId] = key;
        Draw draw{static_cast<uint32_t>(scene.indices.size()), static_cast<uint32_t>(indices.size()), materialIndex, {page, page ? page + 1 : 0, page ? page + 2 : 0, page ? page + 3 : 0}, sky};
        draw.visibility = visibilityMasks.at(visibilityId);
        scene.draws.push_back(draw);
        scene.indices.insert(scene.indices.end(), indices.begin(), indices.end());
    }
    addProps();
    addSky();
    for (auto& draw : scene.draws) {
        glm::vec3 minimum(std::numeric_limits<float>::max()), maximum(std::numeric_limits<float>::lowest());
        for (uint32_t i = 0; i < draw.indexCount; ++i) { auto position = scene.vertices.at(scene.indices.at(draw.firstIndex + i)).position; minimum = glm::min(minimum, position); maximum = glm::max(maximum, position); }
        draw.center = (minimum + maximum) * 0.5f; draw.radius = glm::length(maximum - minimum) * 0.5f;
    }
    scene.diagnostics["vertices"] = scene.vertices.size();
    scene.diagnostics["triangles"] = scene.indices.size() / 3;
    scene.diagnostics["draws"] = scene.draws.size();
    scene.diagnostics["sky_world_faces"] = skyFaces.size();
    glm::vec3 skyMinimum(std::numeric_limits<float>::max()), skyMaximum(std::numeric_limits<float>::lowest());
    size_t skyTriangles = 0;
    for (const auto& draw : scene.draws) { if (draw.skyWorld) { skyTriangles += draw.indexCount / 3; for (uint32_t i = 0; i < draw.indexCount; ++i) { auto position = scene.vertices[scene.indices[draw.firstIndex + i]].position; skyMinimum = glm::min(skyMinimum, position); skyMaximum = glm::max(skyMaximum, position); } } }
    scene.diagnostics["sky_world"] = {{"triangles", skyTriangles}, {"minimum", {skyMinimum.x, skyMinimum.y, skyMinimum.z}}, {"maximum", {skyMaximum.x, skyMaximum.y, skyMaximum.z}}};
    return std::move(scene);
}

void Builder::addProps() {
    auto game = bsp.gameLump(0x73707270);
    if (!game || game->bytes.empty()) { return; }
    const auto& data = game->bytes;
    Reader reader(data, "static props");
    auto models = reader.take<uint32_t>();
    std::vector<std::string> modelNames;
    for (uint32_t i = 0; i < models; ++i) { modelNames.push_back(reader.string(reader.cursor, 128)); reader.cursor += 128; }
    auto leaves = reader.take<uint32_t>();
    reader.slice(reader.cursor, size_t(leaves) * 2);
    reader.cursor += size_t(leaves) * 2;
    auto count = reader.take<uint32_t>();
    if (!count) { return; }
    auto version = game->version;
    require(version >= 4 && version <= 6, std::format("Unsupported static-prop layout v{}.", version));
    size_t stride = version == 4 ? 56 : version == 5 ? 60 : 64;
    require(data.size() - reader.cursor == size_t(count) * stride, "Static-prop byte count does not match its version.");
    auto ambientIndexData = bsp.lump(options.hdr ? 51 : 52), ambientSampleData = bsp.lump(options.hdr ? 55 : 56);
    WorldLighting worldLighting(bsp, options.hdr);
    uint32_t bakedProps = 0, ambientProps = 0;
    Reader ambientIndices(ambientIndexData, "leaf ambient indices"), ambientSamples(ambientSampleData, "leaf ambient samples"), leavesReader(leafData, "prop leaves");
    auto ambientAt = [&](glm::vec3 position) {
        std::array<glm::vec3, 6> cube{};
        int leaf = leafAt(position);
        require(!ambientIndexData.empty() && !ambientSampleData.empty(), "Static-prop lighting requires compiled leaf ambient samples.");
        auto count = ambientIndices.at<uint16_t>(size_t(leaf) * 4), first = ambientIndices.at<uint16_t>(size_t(leaf) * 4 + 2);
        if (!count) {
            require(first != leaf && size_t(first) * 4 < ambientIndexData.size(), "Prop leaf has no usable ambient lighting.");
            leaf = first; count = ambientIndices.at<uint16_t>(size_t(leaf) * 4); first = ambientIndices.at<uint16_t>(size_t(leaf) * 4 + 2);
        }
        require(count > 0, "Prop leaf has no ambient samples.");
        glm::vec3 minimum, maximum;
        for (unsigned axis = 0; axis < 3; ++axis) { minimum[axis] = leavesReader.at<int16_t>(size_t(leaf) * 32 + 8 + axis * 2); maximum[axis] = leavesReader.at<int16_t>(size_t(leaf) * 32 + 14 + axis * 2); }
        float total = 0;
        for (unsigned i = 0; i < count; ++i) {
            size_t offset = (size_t(first) + i) * 28;
            glm::vec3 sample;
            for (unsigned axis = 0; axis < 3; ++axis) { sample[axis] = glm::mix(minimum[axis], maximum[axis], ambientSamples.at<uint8_t>(offset + 24 + axis) / 255.0f); }
            auto delta = sample - position; float weight = 1.0f / (glm::dot(delta, delta) + 1.0f); total += weight;
            for (unsigned side = 0; side < 6; ++side) { cube[side] += rgbExp(ambientSamples, offset + side * 4) * (255.0f * weight); }
        }
        for (auto& side : cube) { side /= total; }
        return cube;
    };
    for (uint32_t i = 0; i < count; ++i) {
        size_t record = reader.cursor + size_t(i) * stride;
        uint32_t propFlags = reader.at<uint8_t>(record + 31);
        if (version >= 6) {
            auto minimum = reader.at<uint16_t>(record + 60), maximum = reader.at<uint16_t>(record + 62);
            if ((minimum && minimum > 95) || (maximum && maximum < 95)) { continue; }
        }
        auto origin = reader.at<glm::vec3>(record), angles = reader.at<glm::vec3>(record + 12);
        auto modelIndex = reader.at<uint16_t>(record + 24);
        require(modelIndex < modelNames.size(), "Static prop references an invalid model.");
        const auto& name = modelNames[modelIndex];
        auto stem = name.substr(0, name.size() - 4);
        auto mdl = assets.get(name, "static prop"), vtx = assets.get(stem + ".dx90.vtx", name), vvd = assets.get(stem + ".vvd", name);
        mdlpp::StudioModel model;
        require(Reader(mdl->bytes, name).at<uint32_t>(8) == Reader(vtx->bytes, name).at<uint32_t>(16) && Reader(mdl->bytes, name).at<uint32_t>(8) == Reader(vvd->bytes, name).at<uint32_t>(8), std::format("Model companion checksums differ: {}", name));
        require(model.open(mdl->bytes, vtx->bytes, vvd->bytes), std::format("Cannot load MDL/VTX/VVD model: {}", name));
        for (const auto& part : model.mdl.bodyParts) { require(part.models.size() <= 1, std::format("Static-prop bodygroups are not supported: {}", name)); }
        auto baked = model.processModelData(0);
        require(baked.vertices.size() <= 65535, std::format("Model exceeds SourcePP's current 16-bit baked index range: {}", name));
        auto matrix = transform(origin, angles);
        auto normalMatrix = glm::mat3(matrix);
        auto lightingOrigin = (propFlags & 2) ? reader.at<glm::vec3>(record + 44) : glm::vec3(matrix * glm::vec4(vec3(model.mdl.illuminationPosition), 1));
        auto ambient = ambientAt(lightingOrigin);
        auto lighting = worldLighting.sample(lightingOrigin); lighting.ambient = ambient;
        std::vector<glm::vec3> bakedColors(baked.vertices.size(), glm::vec3(-1));
        auto lightingAsset = assets.find(std::format("sp_{}{}.vhv", options.hdr ? "hdr_" : "", i));
        if (lightingAsset && !(propFlags & 64)) {
            auto meshes = readVertexLighting(lightingAsset->bytes, Reader(mdl->bytes, name).at<uint32_t>(8));
            size_t meshIndex = 0;
            for (size_t body = 0; body < model.mdl.bodyParts.size(); ++body) {
                const auto& part = model.mdl.bodyParts[body];
                for (size_t m = 0; m < part.models.size(); ++m) {
                    const auto& mdlModel = part.models[m];
                    const auto& lods = model.vtx.bodyParts.at(body).models.at(m).modelLODs;
                    for (size_t lod = 0; lod < lods.size(); ++lod) {
                        require(lods[lod].meshes.size() == mdlModel.meshes.size(), "VTX and MDL mesh counts differ.");
                        for (size_t mesh = 0; mesh < mdlModel.meshes.size(); ++mesh) {
                            for (const auto& group : lods[lod].meshes[mesh].stripGroups) {
                                require(meshIndex < meshes.size(), "VHV is missing a VTX strip group.");
                                const auto& colors = meshes[meshIndex++];
                                require(colors.lod == lod && colors.colors.size() == group.vertices.size(), "VHV LOD or strip-group vertex counts differ from VTX.");
                                if (lod != 0) { continue; }
                                for (size_t vertex = 0; vertex < group.vertices.size(); ++vertex) {
                                    size_t index = size_t(mdlModel.verticesOffset) + mdlModel.meshes[mesh].verticesOffset + group.vertices[vertex].meshVertexID;
                                    require(index < bakedColors.size(), "VHV vertex mapping exceeds model bounds.");
                                    require(bakedColors[index].x < 0 || bakedColors[index] == colors.colors[vertex], "VHV contains conflicting colors for a shared model vertex.");
                                    bakedColors[index] = colors.colors[vertex];
                                }
                            }
                        }
                    }
                }
            }
            require(meshIndex == meshes.size(), "VHV contains strip groups absent from the model.");
            lighting.baked = true; ++bakedProps;
        } else { require(!(propFlags & 8), "Props that ignore lighting normals require baked VHV colors."); ++ambientProps; }
        uint32_t lightingIndex = static_cast<uint32_t>(scene.propLighting.size());
        scene.propLighting.push_back(lighting);
        auto base = static_cast<uint32_t>(scene.vertices.size());
        for (size_t sourceIndex = 0; sourceIndex < baked.vertices.size(); ++sourceIndex) {
            const auto& source = baked.vertices[sourceIndex];
            Vertex vertex;
            vertex.position = glm::vec3(matrix * glm::vec4(vec3(source.position), 1)); vertex.normal = glm::normalize(normalMatrix * vec3(source.normal));
            vertex.tangent = glm::vec4(normalMatrix * glm::vec3(vec4(source.tangent)), source.tangent[3]);
            vertex.uv = {source.uv[0], source.uv[1]};
            glm::vec3 light(0);
            for (unsigned axis = 0; axis < 3; ++axis) { light += ambient[axis * 2 + (vertex.normal[axis] < 0 ? 1 : 0)] * vertex.normal[axis] * vertex.normal[axis]; }
            if (lighting.baked && bakedColors[sourceIndex].x >= 0) { light = bakedColors[sourceIndex]; }
            vertex.color = glm::vec4(light, 1); scene.vertices.push_back(vertex);
        }
        int skin = reader.at<int32_t>(record + 32);
        require(skin >= 0 && size_t(skin) < model.mdl.skins.size(), "Static prop has an invalid model skin.");
        for (const auto& mesh : baked.meshes) {
            auto slot = model.mdl.skins.at(skin).at(mesh.materialIndex);
            const auto& materialName = model.mdl.materials.at(slot).name;
            std::string resolved;
            for (const auto& directory : model.mdl.materialDirectories) {
                auto candidate = assetPath(directory + "/" + materialName);
                if (assets.find("materials/" + candidate + ".vmt")) { resolved = std::move(candidate); break; }
            }
            require(!resolved.empty(), std::format("Missing model material '{}' requested by '{}'. Check mounted content and model material directories.", materialName, name));
            auto id = material(resolved); scene.materials[id].flags |= VertexLit;
            scene.draws.push_back({static_cast<uint32_t>(scene.indices.size()), static_cast<uint32_t>(mesh.indices.size()), id});
            scene.draws.back().lighting = lightingIndex;
            if (skyArea >= 0) { scene.draws.back().skyWorld = (leavesReader.at<uint16_t>(size_t(leafAt(origin)) * 32 + 6) & 511) == skyArea; }
            for (size_t j = 0; j < mesh.indices.size(); j += 3) {
                require(j + 2 < mesh.indices.size(), "Incomplete model triangle.");
                for (auto k : {0u, 2u, 1u}) {
                    auto index = mesh.indices[j + k]; require(index < baked.vertices.size(), "Model index exceeds vertex count.");
                    require(!lighting.baked || bakedColors[index].x >= 0, "VHV is missing a rendered model vertex.");
                    scene.indices.push_back(base + index);
                }
            }
        }
    }
    scene.diagnostics["static_props"] = count;
    scene.diagnostics["prop_lighting"] = {{"vhv", bakedProps}, {"ambient_and_world_lights", ambientProps}};
}

void Builder::addSky() {
    auto info = bsp.source->getLumpData<bsppp::BSPLump::TEXINFO>();
    bool hasSky = std::any_of(info.begin(), info.end(), [](const auto& item) { return (item.flags & 6) != 0; });
    if (!hasSky && !options.defaultSky) { return; }
    std::string name;
    for (const auto& entity : bsp.entities) { if (entity["classname"].getValue() == "worldspawn") { name = entity["skyname"].getValue(); } }
    require(!name.empty(), "Sky surfaces exist but worldspawn has no skyname.");
    if (name == "painted") {
        auto& sky = scene.paintedSky;
        for (const auto& entity : bsp.entities) {
            if (entity["classname"].getValue() != "env_skypaint") { continue; }
            require(!sky.enabled, "Multiple env_skypaint entities require runtime selection.");
            sky.enabled = true;
            auto number = [&](const char* key, float fallback) { return parseFloat(entity[key].getValue(), fallback); };
            auto vector = [&](const char* key, glm::vec3 fallback) { return parseVector(entity[key].getValue(), fallback); };
            sky.top = vector("topcolor", sky.top); sky.bottom = vector("bottomcolor", sky.bottom);
            sky.sunDirection = vector("sunnormal", sky.sunDirection); sky.sunColor = vector("suncolor", sky.sunColor); sky.duskColor = vector("duskcolor", sky.duskColor);
            sky.fadeBias = number("fadebias", sky.fadeBias); sky.hdrScale = number("hdrscale", sky.hdrScale);
            sky.sunSize = number("sunsize", sky.sunSize); sky.duskScale = number("duskscale", sky.duskScale); sky.duskIntensity = number("duskintensity", sky.duskIntensity);
            require(sky.fadeBias >= 0 && sky.hdrScale >= 0, "Painted sky has invalid fade or HDR parameters.");
            require(sky.sunSize == 0 && sky.duskIntensity == 0, "Painted sky sun discs and dusk bands are not implemented yet.");
            float layers = number("drawstars", 1) != 0 ? number("starlayers", 1) : 0;
            require(layers == 0 || layers == 1, "Painted skies currently support one star/cloud layer.");
            sky.starLayers = static_cast<uint32_t>(layers);
            sky.starScale = number("starscale", sky.starScale); sky.starFade = number("starfade", sky.starFade);
            sky.starPosition = number("starspeed", 0.01f) * static_cast<float>(options.time);
            require(sky.starScale > 0 && sky.starFade > 0 && std::isfinite(sky.starPosition), "Painted sky star scale, fade, and time must be finite and valid.");
            if (sky.starLayers) {
                auto star = std::string(entity["startexture"].getValue());
                sky.stars = texture(star.empty() ? "skybox/starfield" : star, false, "env_skypaint");
                require(options.experimental, "Painted sky clouds require --experimental until native sampling comparison passes.");
                scene.diagnostics["warnings"].push_back("Painted sky cloud sampling is experimental; full-face native parity is not certified.");
            }
        }
        require(sky.enabled, "The painted sky requires an env_skypaint entity.");
        scene.diagnostics["painted_sky"] = {{"capture_time", options.time}, {"star_layers", sky.starLayers}, {"hdr_scale", options.hdr ? sky.hdrScale : 1.0f}};
    }
    const std::array<std::string, 6> suffix{"rt", "lf", "bk", "ft", "up", "dn"};
    const std::array<glm::vec3, 6> forward{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
    const std::array<glm::vec3, 6> up{{{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}}};
    for (unsigned face = 0; face < 6; ++face) {
        auto id = material("skybox/" + name + suffix[face]);
        scene.materials[id].flags |= Unlit | Sky;
        scene.materials[id].noCull = true;
        if (scene.paintedSky.enabled) { scene.materials[id].second = scene.paintedSky.stars; }
        auto right = glm::cross(up[face], forward[face]);
        uint32_t base = static_cast<uint32_t>(scene.vertices.size());
        for (auto uv : std::array<glm::vec2, 4>{{{0,0},{1,0},{1,1},{0,1}}}) {
            Vertex vertex;
            vertex.position = (forward[face] + right * (2 * uv.x - 1) + up[face] * (1 - 2 * uv.y)) * 1000.0f;
            vertex.uv = uv;
            scene.vertices.push_back(vertex);
        }
        scene.draws.push_back({static_cast<uint32_t>(scene.indices.size()), 6, id, {}, false, true});
        for (auto index : {0u,1u,2u,0u,2u,3u}) { scene.indices.push_back(base + index); }
    }
}
}
Scene loadScene(const Bsp& bsp, Assets& assets, const SceneOptions& options) {
    auto key = sceneCacheKey(bsp, assets, options);
    auto file = options.cache / "scenes" / (key + ".bin");
    if (options.useCache && !options.cache.empty()) { if (auto scene = readSceneCache(file, assets)) { return std::move(*scene); } }
    auto scene = Builder(bsp, assets, options).build(); scene.diagnostics["scene_cache_hit"] = false;
    if (options.useCache && !options.cache.empty()) { try { writeSceneCache(file, scene, assets); } catch (const std::exception& error) { spdlog::warn("Scene cache write failed: {}", error.what()); } }
    return scene;
}
}
