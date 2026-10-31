#include <scm/scene.hpp>
#include <scm/cache.hpp>
#include <scm/lighting.hpp>
#include <scm/entities.hpp>
#include <vtfpp/VTF.h>
#include <mdlpp/mdlpp.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>
#include <bit>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace scm {
namespace {
template<class T> std::vector<T> records(std::span<const std::byte> bytes, std::string_view label) {
    require(bytes.size() % sizeof(T) == 0, "Unsupported {} record layout.", label);
    std::vector<T> result(bytes.size() / sizeof(T));
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}
template<class V> glm::vec3 vec3(const V& value) { return {value[0], value[1], value[2]}; }
template<class V> glm::vec4 vec4(const V& value) { return {value[0], value[1], value[2], value[3]}; }
float linearColor(float value) { return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f); }
/// Linearizes sRGB pixels, rejects nonfinite values, and stores half floats unless a value needs full precision.
void storeFloatPixels(Image& image, std::vector<glm::vec4> pixels) {
    bool halfPrecision = true;
    for (auto& pixel : pixels) {
        if (image.srgb) { pixel.r = linearColor(pixel.r); pixel.g = linearColor(pixel.g); pixel.b = linearColor(pixel.b); }
        for (unsigned channel = 0; channel < 4; ++channel) {
            require(std::isfinite(pixel[channel]), "Texture '{}' contains non-finite pixels.", image.name);
            halfPrecision &= std::abs(pixel[channel]) <= 65504.0f;
        }
    }
    image.srgb = false;
    image.format = halfPrecision ? PixelFormat::RGBA16F : PixelFormat::RGBA32F;
    if (!halfPrecision) { auto bytes = std::as_bytes(std::span(pixels)); image.pixels.assign(bytes.begin(), bytes.end()); return; }
    image.pixels.resize(pixels.size() * 8);
    auto* packed = reinterpret_cast<uint32_t*>(image.pixels.data());
    for (size_t i = 0; i < pixels.size(); ++i) {
        packed[i * 2] = glm::packHalf2x16(glm::vec2(pixels[i].r, pixels[i].g));
        packed[i * 2 + 1] = glm::packHalf2x16(glm::vec2(pixels[i].b, pixels[i].a));
    }
}
/// Returns the upload format whose bytes match a VTF format exactly, when one exists.
std::optional<PixelFormat> directFormat(vtfpp::ImageFormat format) {
    using enum vtfpp::ImageFormat;
    switch (format) {
        case DXT1: case DXT1_ONE_BIT_ALPHA: return PixelFormat::BC1;
        case DXT3: return PixelFormat::BC2;
        case DXT5: return PixelFormat::BC3;
        case RGBA8888: return PixelFormat::RGBA8;
        case BGRA8888: return PixelFormat::BGRA8;
        case RGBA16161616F: return PixelFormat::RGBA16F;
        default: return {};
    }
}
const auto lightExponentScales = [] {
    std::array<float, 256> scales{};
    for (int i = 0; i < 256; ++i) { scales[i] = std::ldexp(1.0f / 255.0f, int(int8_t(uint8_t(i)))); }
    return scales;
}();
glm::vec3 rgbExp(const std::byte* sample) {
    return glm::vec3(uint8_t(sample[0]), uint8_t(sample[1]), uint8_t(sample[2])) * lightExponentScales[uint8_t(sample[3])];
}
glm::vec3 rgbExp(const Reader& reader, size_t offset) { return rgbExp(reader.slice(offset, 4).data()); }
glm::mat4 transform(glm::vec3 origin, glm::vec3 angles) {
    auto matrix = glm::translate(glm::mat4(1), origin);
    matrix = glm::rotate(matrix, glm::radians(angles.y), glm::vec3(0, 0, 1));
    matrix = glm::rotate(matrix, glm::radians(angles.x), glm::vec3(0, 1, 0));
    return glm::rotate(matrix, glm::radians(angles.z), glm::vec3(1, 0, 0));
}
glm::vec2 materialPair(std::string text, glm::vec2 fallback) {
    if (text.empty()) { return fallback; }
    for (auto& character : text) { if (character == '[' || character == ']') { character = ' '; } }
    std::istringstream stream(text); glm::vec2 result; std::string remaining;
    require(bool(stream >> result.x >> result.y) && !(stream >> remaining) && std::isfinite(result.x) && std::isfinite(result.y), "Invalid two-component material parameter.");
    return result;
}
glm::vec3 materialColor(std::string text, glm::vec3 fallback = glm::vec3(1)) {
    auto result = parseVector(text, fallback);
    return text.find('{') != std::string::npos ? result / 255.0f : result;
}
WaterMaterial waterDefinition(const MaterialDefinition& definition, double time) {
    WaterMaterial water;
    water.above = definition.flag("$abovewater");
    water.reflection = !definition.value("$reflecttexture").empty();
    require(definition.value("$refracttexture") == "_rt_WaterRefraction", "Water requires the standard refraction render target.");
    require(!water.reflection || definition.value("$reflecttexture") == "_rt_WaterReflection", "Water requires the standard reflection render target.");
    require(!definition.flag("$forcecheap") && !definition.flag("$blurrefract") && definition.value("$basetexture").empty(), "Cheap, blurred, and base-textured water are not implemented.");
    require(definition.flag("$fogenable"), "Water without depth fog is not supported.");
    water.fogColor = materialColor(definition.value("$fogcolor"));
    water.reflectTint = materialColor(definition.value("$reflecttint"));
    water.refractTint = materialColor(definition.value("$refracttint"));
    water.fogStart = parseFloat(definition.value("$fogstart")); water.fogEnd = parseFloat(definition.value("$fogend"), 1024);
    require(water.fogEnd > water.fogStart, "Water fog end must exceed its start.");
    water.reflectAmount = parseFloat(definition.value("$reflectamount"), 0.8f); water.refractAmount = parseFloat(definition.value("$refractamount"));
    require(water.reflectAmount >= 0 && water.refractAmount >= 0, "Water distortion cannot be negative.");
    water.reflectEntities = definition.flag("$reflectentities"); water.noFresnel = definition.flag("$nofresnel");
    water.scale = materialPair(definition.value("$scale"), glm::vec2(1));
    water.scroll1 = parseVector(definition.value("$scroll1")); water.scroll2 = parseVector(definition.value("$scroll2"));
    require(definition.proxies.size() == definition.proxyParameters.size(), "Missing water proxy parameters.");
    bool scrollFound = false;
    for (size_t i = 0; i < definition.proxies.size(); ++i) {
        auto name = lower(definition.proxies[i]);
        if (name == "waterlod") { continue; }
        require(name == "texturescroll" && !scrollFound, "Unsupported water proxy '{}'.", name);
        scrollFound = true;
        const auto& parameters = definition.proxyParameters[i];
        auto value = [&](const char* key) { auto found = parameters.find(key); return found == parameters.end() ? std::string{} : found->second; };
        require(lower(value("texturescrollvar")) == "$bumptransform", "Water TextureScroll must target $bumptransform.");
        float angle = glm::radians(parseFloat(value("texturescrollangle"))), rate = parseFloat(value("texturescrollrate"));
        water.scroll = glm::vec2(std::cos(angle), std::sin(angle)) * static_cast<float>(rate * time);
        require(std::isfinite(water.scroll.x) && std::isfinite(water.scroll.y), "Water capture time exceeds the supported range.");
    }
    return water;
}
/// A material texture transform parsed once so applying it to each vertex costs only arithmetic.
class UvTransform {
public:
    explicit UvTransform(std::string_view description = {}) {
        if (description.empty()) { return; }
        std::istringstream stream{std::string(description)};
        std::string key;
        float angle = 0;
        while (stream >> key) {
            if (key == "center") { stream >> center.x >> center.y; }
            else if (key == "scale") { stream >> scale.x >> scale.y; }
            else if (key == "rotate") { stream >> angle; }
            else if (key == "translate") { stream >> translation.x >> translation.y; }
            else { throw Error(std::format("Unsupported texture transform: {}", description)); }
            require(!stream.fail() && std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(scale.x) && std::isfinite(scale.y) && std::isfinite(angle) && std::isfinite(translation.x) && std::isfinite(translation.y), "Invalid texture transform: {}", description);
        }
        float radians = glm::radians(angle);
        cosine = std::cos(radians); sine = std::sin(radians); identity = false;
    }
    glm::vec2 operator()(glm::vec2 uv) const {
        if (identity) { return uv; }
        uv = (uv - center) * scale;
        return glm::vec2(cosine * uv.x - sine * uv.y, sine * uv.x + cosine * uv.y) + center + translation;
    }
private:
    bool identity = true;
    glm::vec2 center{0.5f}, scale{1}, translation{0};
    float cosine = 1, sine = 0;
};
struct MaterialUv { UvTransform secondary, blend, base; };
/// The set of map faces reachable from one point, addressed by the 16-bit indices of the leaf-face lump.
class FaceSet {
public:
    void insert(uint32_t face) { words[face >> 6] |= uint64_t(1) << (face & 63); }
    bool contains(size_t face) const { return face < 65536 && ((words[face >> 6] >> (face & 63)) & 1) != 0; }
    size_t size() const {
        size_t count = 0;
        for (auto word : words) { count += static_cast<size_t>(std::popcount(word)); }
        return count;
    }
private:
    std::array<uint64_t, 1024> words{};
};

/// A texture whose asset is resolved and whose VTF decoding waits for the parallel pass at the end of the build.
struct PendingTexture { uint32_t index; std::shared_ptr<const Asset> asset; bool srgb; int frame; };
class Builder {
public:
    Builder(const Bsp& input, Assets& resolver, const SceneOptions& settings) : bsp(input), assets(resolver), options(settings) {
        scene.hdr = options.hdr;
        scene.captureTime = options.time;
        scene.probes = bsp.probes;
        scene.images.push_back(solidImage("white", glm::vec4(1)));
        scene.images.push_back(solidImage("flat normal", glm::vec4(0.5f, 0.5f, 1, 1)));
        scene.diagnostics = {{"unsupported", Json::array()}, {"warnings", Json::array()}};
        lightStyles = initialLightStyles(bsp);
    }
    uint32_t texture(std::string name, bool srgb, std::string_view requester, int frame = -1) {
        if (name.empty()) { return 0; }
        name = assetPath(name);
        if (!name.starts_with("materials/")) { name = "materials/" + name; }
        if (!name.ends_with(".vtf")) { name += ".vtf"; }
        std::string key = name + (srgb ? ":srgb" : ":linear") + std::format(":{}", frame);
        if (textureIds.contains(key)) { return textureIds.at(key); }
        auto asset = assets.get(name, requester);
        uint32_t index = static_cast<uint32_t>(scene.images.size());
        scene.images.push_back({name});
        pendingTextures.push_back({index, std::move(asset), srgb, frame});
        textureIds.emplace(key, index);
        return index;
    }
    void decodeTexture(const PendingTexture& pending) {
        auto& image = scene.images[pending.index];
        const auto& name = image.name;
        const auto& bytes = pending.asset->bytes;
        validateVtf(bytes, name);
        vtfpp::VTF vtf(bytes, false, options.hdr);
        require(static_cast<bool>(vtf), "Invalid texture {}.", name);
        require(vtf.getFrameCount() == 1 || pending.frame >= 0, "Animated VTF is not supported yet: {}", name);
        require(std::max(pending.frame, 0) < vtf.getFrameCount(), "Texture frame is out of range: {}", name);
        require(vtf.getFaceCount() == 1 && vtf.getDepth() == 1, "Expected a 2D texture: {}", name);
        auto format = vtf.getFormat();
        bool floating = format == vtfpp::ImageFormat::RGBA16161616F || format == vtfpp::ImageFormat::RGB323232F || format == vtfpp::ImageFormat::RGBA32323232F;
        image.width = vtf.getWidth(); image.height = vtf.getHeight(); image.srgb = pending.srgb && !floating;
        image.mipCount = vtf.getMipCount();
        image.clamp = (vtf.getFlags() & (vtfpp::VTF::FLAG_V0_CLAMP_S | vtfpp::VTF::FLAG_V0_CLAMP_T)) != 0;
        auto frame = static_cast<uint16_t>(std::max(pending.frame, 0));
        auto mipWidth = [&](uint32_t mip) { return std::max(1u, image.width >> mip); };
        auto mipHeight = [&](uint32_t mip) { return std::max(1u, image.height >> mip); };
        auto direct = directFormat(format);
        if (direct || vtfpp::ImageFormatDetails::containerFormat(format) == vtfpp::ImageFormat::RGBA8888) {
            image.format = direct.value_or(PixelFormat::RGBA8);
            size_t total = 0;
            for (uint32_t mip = 0; mip < image.mipCount; ++mip) { total += pixelBytes(image.format, mipWidth(mip), mipHeight(mip)); }
            require(total <= (1ull << 30), "Decoded texture '{}' exceeds the 1 GiB limit.", name);
            image.pixels.reserve(total);
            for (uint32_t mip = 0; mip < image.mipCount; ++mip) {
                auto expected = pixelBytes(image.format, mipWidth(mip), mipHeight(mip));
                if (direct) {
                    auto raw = vtf.getImageDataRaw(static_cast<uint8_t>(mip), frame);
                    require(raw.size() == expected, "Failed to decode mip {} of {}.", mip, name);
                    image.pixels.insert(image.pixels.end(), raw.begin(), raw.end());
                } else {
                    auto converted = vtf.getImageDataAs(vtfpp::ImageFormat::RGBA8888, static_cast<uint8_t>(mip), frame);
                    require(converted.size() == expected, "Failed to decode mip {} of {}.", mip, name);
                    image.pixels.insert(image.pixels.end(), converted.begin(), converted.end());
                }
            }
            if (image.format == PixelFormat::RGBA16F) {
                std::span halves(reinterpret_cast<const uint16_t*>(image.pixels.data()), image.pixels.size() / 2);
                require(std::ranges::none_of(halves, [](uint16_t value) { return (value & 0x7c00) == 0x7c00; }), "Texture '{}' contains non-finite pixels.", name);
            }
            return;
        }
        std::vector<glm::vec4> pixels;
        for (uint32_t mip = 0; mip < image.mipCount; ++mip) {
            size_t count = size_t(mipWidth(mip)) * mipHeight(mip);
            require((pixels.size() + count) * sizeof(glm::vec4) <= (1ull << 30), "Decoded texture '{}' exceeds the 1 GiB limit.", name);
            auto converted = vtf.getImageDataAs(vtfpp::ImageFormat::RGBA32323232F, static_cast<uint8_t>(mip), frame);
            require(converted.size() == count * sizeof(glm::vec4), "Failed to decode mip {} of {}.", mip, name);
            auto offset = pixels.size(); pixels.resize(offset + count);
            std::memcpy(pixels.data() + offset, converted.data(), converted.size());
        }
        storeFloatPixels(image, std::move(pixels));
    }
    void finishImages() {
        parallelFor(pendingTextures.size(), [&](size_t i) { decodeTexture(pendingTextures[i]); });
        parallelFor(lightmapPages.size(), [&](size_t i) { storeFloatPixels(scene.images[lightmapPages[i].first], std::move(lightmapPages[i].second)); });
    }
    uint32_t material(std::string name) {
        name = lower(std::move(name));
        if (materialIds.contains(name)) { return materialIds.at(name); }
        Material material;
        material.name = name;
        auto& definition = material.definition = readMaterial(assets, name, options.hdr);
        auto shader = definition.shader;
        if (shader == "sdk_lightmappedgeneric" || shader == "sdk_unlitgeneric" || shader == "sdk_vertexlitgeneric" || shader == "sdk_worldvertextransition") {
            require(options.experimental, "SDK shader '{}' in '{}' requires --experimental; custom shader extensions are not certified.", shader, name);
            shader.erase(0, 4);
            scene.diagnostics["warnings"].push_back(std::format("SDK shader '{}' in '{}' uses the supported '{}' parameters; custom extensions may differ.", definition.shader, name, shader));
        }
        if (shader == "lightmappedreflective") {
            require(options.experimental, "Planar glass reflections are not implemented for '{}'.", name);
            scene.diagnostics["warnings"].push_back(std::format("Planar reflective glass '{}' is omitted in experimental captures.", name));
            material.flags = Omitted;
            uint32_t index = static_cast<uint32_t>(scene.materials.size()); scene.materials.push_back(std::move(material)); materialIds.emplace(name, index); return index;
        }
        if (shader == "water") {
            require(options.planarWater && options.experimental, "Water '{}' requires --water planar --experimental. Native water parity is not certified.", name);
            material.water = waterDefinition(definition, options.time);
            float frame = parseFloat(definition.value("$bumpframe"));
            require(frame >= 0 && frame <= 65535 && std::floor(frame) == frame, "Invalid water normal frame.");
            material.normal = texture(definition.value("$normalmap", "dev/water_normal"), false, name, static_cast<int>(frame));
            material.flags = Water | Unlit | NormalMap;
            material.noCull = true;
            uint32_t index = static_cast<uint32_t>(scene.materials.size());
            scene.materials.push_back(std::move(material)); materialIds.emplace(name, index);
            return index;
        }
        bool painted = shader == "g_sky";
        require(shader == "lightmappedgeneric" || shader == "unlitgeneric" || shader == "unlittwotexture" || shader == "worldvertextransition" || shader == "vertexlitgeneric" || shader == "sky" || painted, "Unsupported shader '{}' in '{}'. No BSP was written.", shader, name);
        int baseFrame = -1;
        if (!definition.value("$frame").empty()) {
            auto frame = parseFloat(definition.value("$frame"));
            require(frame >= 0 && frame <= 65535 && std::floor(frame) == frame, "Invalid authored texture frame."); baseFrame = static_cast<int>(frame);
        }
        for (size_t i = 0; i < definition.proxies.size(); ++i) {
            auto proxy = lower(definition.proxies[i]);
            if (painted && proxy == "skypaint") { continue; }
            if (proxy == "texturescroll") {
                const auto& parameters = definition.proxyParameters.at(i);
                require(parameters.contains("texturescrollvar"), "TextureScroll proxy has no target variable.");
                auto target = lower(parameters.at("texturescrollvar"));
                require(target == "$basetexturetransform" || target == "$texture2transform", "Unsupported TextureScroll target '{}' in '{}'.", target, name);
                auto scalar = [&](const std::string& key, float fallback) {
                    auto found = parameters.find(key);
                    if (found == parameters.end()) { return fallback; }
                    auto value = found->second;
                    if (value.starts_with('$')) { value = definition.value(lower(value)); require(!value.empty(), "TextureScroll references an undefined parameter."); }
                    return parseFloat(value);
                };
                auto scale = scalar("texturescale", 1), angle = glm::radians(scalar("texturescrollangle", 0));
                double distance = options.time * scalar("texturescrollrate", 1), x = distance * std::cos(angle), y = distance * std::sin(angle);
                definition.parameters[target] = std::format("center 0 0 scale {} {} translate {} {}", scale, scale, x - std::floor(x), y - std::floor(y));
                continue;
            }
            if (proxy == "sine") {
                const auto& parameters = definition.proxyParameters.at(i);
                auto scalar = [&](const std::string& key, float fallback) {
                    auto found = parameters.find(key);
                    if (found == parameters.end()) { return fallback; }
                    auto value = found->second;
                    if (value.starts_with('$')) { value = definition.value(lower(value)); require(!value.empty(), "Sine proxy references an undefined material parameter."); }
                    return parseFloat(value);
                };
                auto result = parameters.find("resultvar"); require(result != parameters.end(), "Sine proxy has no result variable.");
                auto target = lower(result->second);
                int component = -1;
                for (int channel = 0; channel < 3; ++channel) {
                    if (target.ends_with(std::format("[{}]", channel))) { component = channel; target.resize(target.size() - 3); break; }
                }
                require(target == "$selfillumtint" || target == "$color" || target == "$color2" || target == "$alpha" || target == "$envmaptint", "Unsupported Sine result '{}' in '{}'.", target, name);
                auto period = scalar("sineperiod", 1); if (period == 0) { period = 1; }
                auto minimum = scalar("sinemin", 0), maximum = scalar("sinemax", 1);
                double phase = (options.time - scalar("timeoffset", 0)) / period;
                double value = minimum + (maximum - minimum) * (0.5 + 0.5 * std::sin(phase * 6.283185307179586));
                require(target != "$alpha" || component == -1, "Scalar alpha cannot have a vector component.");
                auto vector = component == -1 ? glm::vec3(static_cast<float>(value)) : parseVector(definition.value(target), glm::vec3(1));
                if (component != -1) { vector[component] = static_cast<float>(value); }
                definition.parameters[target] = target == "$alpha" ? std::format("{}", value) : std::format("[{} {} {}]", vector.x, vector.y, vector.z);
                continue;
            }
            if (proxy == "toggletexture") {
                require(options.experimental && options.time == 0, "Entity texture toggles in '{}' require --experimental at capture time zero; entity inputs are not simulated.", name);
                const auto& parameters = definition.proxyParameters.at(i);
                require(parameters.contains("toggletexturevar") && lower(parameters.at("toggletexturevar")) == "$basetexture" && parameters.contains("toggletextureframenumvar") && lower(parameters.at("toggletextureframenumvar")) == "$frame", "Only base-texture frame toggles are implemented.");
                baseFrame = std::max(baseFrame, 0);
                scene.diagnostics["warnings"].push_back(std::format("ToggleTexture in '{}' uses authored frame {}; runtime entity inputs are not simulated.", name, baseFrame));
                continue;
            }
            throw Error(std::format("Material proxy '{}' is not implemented for '{}'. No BSP was written.", definition.proxies[i], name));
        }
        if (shader == "unlitgeneric" || shader == "unlittwotexture" || shader == "sky" || painted) { material.flags |= Unlit; }
        if (shader == "unlittwotexture") { material.flags |= TwoTexture; require(!definition.flag("$cloakpassenabled"), "UnlitTwoTexture cloak passes are not implemented."); }
        if (shader == "worldvertextransition") { material.flags |= Blend; }
        if (shader == "vertexlitgeneric") { material.flags |= VertexLit; }
        if (painted) { material.flags |= Painted; }
        for (auto parameter : {"$bumpmap2", "$lightwarptexture", "$selfillumfresnel", "$selfillum_envmapmask_alpha", "$seamless_scale", "$detailtexturetransform"}) {
            require(definition.value(parameter).empty() || definition.value(parameter) == "0", "Material parameter '{}' is not implemented for '{}'.", parameter, name);
        }
        require(definition.value("$bumptransform").empty() || definition.value("$bumptransform") == definition.value("$basetexturetransform"), "Independent bump texture coordinates are not implemented for '{}'.", name);
        auto environment = definition.value("$envmap");
        require(environment.empty() || lower(environment) == "env_cubemap" || assetPath(environment).starts_with("maps/"), "Static environment textures are not implemented for '{}'. Only probe-driven reflection feedback can be suppressed.", name);
        auto base = definition.value(options.hdr ? "$hdrbasetexture" : "$basetexture");
        if (base.empty()) { base = definition.value("$basetexture"); }
        material.base = texture(base, true, name, baseFrame);
        material.second = texture(definition.value(shader == "unlittwotexture" ? "$texture2" : "$basetexture2"), true, name);
        if (!definition.value("$blendmodulatetexture").empty()) {
            require(shader == "worldvertextransition" && !definition.flag("$maskedblending"), "Blend modulation currently requires ordinary WorldVertexTransition blending.");
            material.blendMask = texture(definition.value("$blendmodulatetexture"), false, name); material.flags |= BlendModulation;
        }
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
        require(!(material.flags & Detail) || material.detailMode == 0, "Detail blend mode {} is not implemented for {}.", material.detailMode, name);
        material.tint = glm::vec4(parseVector(definition.value("$color", definition.value("$color2")), glm::vec3(1)), parseFloat(definition.value("$alpha"), 1));
        if (definition.flag("$ssbump")) { require(material.flags & NormalMap, "SSBump requires a bump texture."); material.flags |= SSBump; }
        material.selfIllumTint = parseVector(definition.value("$selfillumtint"), glm::vec3(1));
        require(glm::all(glm::greaterThanEqual(material.selfIllumTint, glm::vec3(0))), "Self-illumination tint cannot be negative.");
        if (definition.flag("$selfillum") && !definition.value("$selfillummask").empty()) {
            require(shader == "vertexlitgeneric" && !(material.flags & NormalMap), "Separate self-illumination masks currently require an unbumped VertexLitGeneric material: {}", name);
            material.selfIllumMask = texture(definition.value("$selfillummask"), false, name); material.flags |= SelfIllumMask;
        }
        if (definition.flag("$phong")) {
            require(shader == "vertexlitgeneric", "Phong is supported only for VertexLitGeneric: {}", name);
            for (auto key : {"$phongwarptexture", "$rimlight", "$phongexponenttexturemask"}) {
                require(definition.value(key).empty() || definition.value(key) == "0", "Unsupported Phong control '{}' in '{}'.", key, name);
            }
            material.flags |= Phong;
            if (!definition.flag("$phongdisablehalflambert")) { material.flags |= HalfLambert; }
            material.phongExponent = parseFloat(definition.value("$phongexponent"), 5);
            material.phongBoost = parseFloat(definition.value("$phongboost"), 1);
            material.phongExponentFactor = parseFloat(definition.value("$phongexponentfactor"), 149);
            material.phongFresnel = parseVector(definition.value("$phongfresnelranges"), {0, 0.5f, 1});
            material.phongTint = parseVector(definition.value("$phongtint"), glm::vec3(1));
            require(material.phongExponent > 0 && material.phongBoost >= 0 && material.phongExponentFactor >= 0, "Invalid Phong parameters in '{}'.", name);
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
    struct LeafFaces { uint16_t first, count; };
    void indexLeaves() {
        Reader leaves(leafData, "leaves");
        clusterLeaves.clear(); unclusteredLeaves.clear();
        for (size_t i = 0; i < leafData.size() / 32; ++i) {
            int cluster = leaves.at<int16_t>(i * 32 + 4);
            LeafFaces range{leaves.at<uint16_t>(i * 32 + 20), leaves.at<uint16_t>(i * 32 + 22)};
            if (!range.count) { continue; }
            if (cluster < 0) { unclusteredLeaves.push_back(range); continue; }
            if (size_t(cluster) >= clusterLeaves.size()) { clusterLeaves.resize(size_t(cluster) + 1); }
            clusterLeaves[cluster].push_back(range);
        }
    }
    FaceSet visibleFaces(glm::vec3 point) const {
        Reader leaves(leafData, "leaves"), vis(visData, "PVS"), faceRefs(leafFaceData, "leaf faces");
        FaceSet result;
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
        auto addFaces = [&](const LeafFaces& range) { for (size_t j = 0; j < range.count; ++j) { result.insert(faceRefs.at<uint16_t>((range.first + j) * 2)); } };
        for (size_t candidate = 0; candidate < clusterLeaves.size(); ++candidate) {
            if (!visible.empty() && (candidate / 8 >= visible.size() || !(visible[candidate / 8] & (1 << (candidate % 8))))) { continue; }
            for (const auto& range : clusterLeaves[candidate]) { addFaces(range); }
        }
        if (visible.empty()) { for (const auto& range : unclusteredLeaves) { addFaces(range); } }
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
                scene.images.push_back({std::format("lightmap-{}-{}", pageIds[0], i), 1024, 1024, false, true});
                pagePixels[i] = lightmapPages.emplace_back(pageIds[i], std::vector<glm::vec4>(1024 * 1024, glm::vec4(0, 0, 0, 1))).second.data();
            }
        }
        LightPlacement result{pageIds, glm::vec2(pageX + 1, pageY + 1), glm::vec2(width, height)};
        lightmapTasks.push_back({&face, pagePixels, width, height, pageX, pageY, styleCount, bumped});
        pageX += width + 2; pageRow = std::max(pageRow, height + 2);
        return result;
    }
    struct LightmapTask {
        const bsppp::BSPFace_v1* face;
        std::array<glm::vec4*, 4> pages;
        uint32_t width, height, pageX, pageY;
        size_t styleCount;
        bool bumped;
    };
    void fillLightmap(const LightmapTask& task) {
        const auto& [face, pages, width, height, pageX, pageY, styleCount, bumped] = task;
        size_t planes = bumped ? 4 : 1, luxels = size_t(width) * height;
        auto source = Reader(lightData, "lightmaps").slice(size_t(face->lightOffset), styleCount * planes * luxels * 4).data();
        std::vector<std::array<glm::vec3, 4>> luxelRadiance(luxels);
        for (size_t pixel = 0; pixel < luxels; ++pixel) {
            std::array<glm::vec3, 4> samples{};
            for (size_t plane = 0; plane < planes; ++plane) {
                for (size_t style = 0; style < styleCount; ++style) { samples[plane] += rgbExp(source + ((style * planes + plane) * luxels + pixel) * 4) * lightStyles[face->styles[style]]; }
            }
            if (!bumped) { samples[1] = samples[2] = samples[3] = samples[0]; }
            luxelRadiance[pixel] = lightmapRadiance(samples, bumped, options.hdr);
        }
        for (int y = -1; y <= int(height); ++y) {
            for (int x = -1; x <= int(width); ++x) {
                const auto& radiance = luxelRadiance[uint32_t(std::clamp(y, 0, int(height) - 1)) * width + uint32_t(std::clamp(x, 0, int(width) - 1))];
                for (unsigned plane = 0; plane < 4; ++plane) { pages[plane][(pageY + 1 + y) * 1024 + pageX + 1 + x] = glm::vec4(radiance[plane], 1); }
            }
        }
    }
    Scene build();
    void addProps();
    void addSky();
    Scene scene;
    const Bsp& bsp;
    Assets& assets;
    SceneOptions options;
    const MaterialUv& materialUv(uint32_t index) {
        if (uvTransforms.size() <= index) { uvTransforms.resize(scene.materials.size()); }
        auto& transforms = uvTransforms[index];
        if (!transforms) {
            const auto& material = scene.materials[index];
            const auto& definition = material.definition;
            transforms = std::make_unique<MaterialUv>(MaterialUv{UvTransform(definition.value((material.flags & TwoTexture) ? "$texture2transform" : "$basetexturetransform2")), UvTransform(definition.value("$blendmasktransform")), UvTransform(definition.value("$basetexturetransform"))});
        }
        return *transforms;
    }
    std::span<const std::byte> nodesData, planeData, leafData, leafFaceData, visData, lightData;
    std::vector<std::vector<LeafFaces>> clusterLeaves;
    std::vector<LeafFaces> unclusteredLeaves;
    std::vector<LightmapTask> lightmapTasks;
    std::vector<std::unique_ptr<MaterialUv>> uvTransforms;
    FaceSet skyFaces;
    int skyArea = -1;
    std::map<std::string, uint32_t> textureIds, materialIds;
    std::vector<PendingTexture> pendingTextures;
    std::vector<std::pair<uint32_t, std::vector<glm::vec4>>> lightmapPages;
    std::array<glm::vec4*, 4> pagePixels{};
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
    std::vector<int> sceneLumps{1, 2, 3, 4, 5, 6, 10, 12, 13, 14, 16, 17, 18, 19, 26, 33, 36, 37, 38, 39, 43, 44};
    sceneLumps.insert(sceneLumps.end(), {options.hdr ? 53 : 8, options.hdr ? 54 : 15, options.hdr ? 51 : 52, options.hdr ? 55 : 56, options.hdr && bsp.lumps[58].length ? 58 : 7});
    bsp.decodeLumps(sceneLumps);
    require(bsp.lumps[7].version == 1 && bsp.lumps[10].version == 1, "Only face v1 and leaf v1 layouts are supported.");
    if (options.hdr && bsp.lumps[58].length) { require(bsp.lumps[58].version == 1, "Unsupported HDR face layout."); }
    for (auto [id, stride] : std::array<std::pair<int, size_t>, 17>{{{1,20},{2,32},{3,12},{5,32},{6,72},{12,4},{13,4},{14,48},{16,2},{26,176},{33,20},{37,10},{38,12},{39,2},{43,1},{44,4},{48,2}}}) {
        require(bsp.lumps[id].version == 0 && bsp.view(id).size() % stride == 0, "Unsupported record version or stride in lump {}.", id);
    }
    require(bsp.view(10).size() % 32 == 0, "Leaf data has a partial record.");
    auto faces = records<bsppp::BSPFace_v1>(bsp.view(options.hdr && bsp.lumps[58].length ? 58 : 7), "faces");
    auto points = records<bsppp::BSPVertex_v0>(bsp.view(3), "vertices");
    auto edges = records<bsppp::BSPEdge_v0>(bsp.view(12), "edges");
    auto surfEdges = records<bsppp::BSPSurfEdge_v0>(bsp.view(13), "surface edges");
    auto texInfo = records<bsppp::BSPTextureInfo_v0>(bsp.view(6), "texture information");
    auto texData = records<bsppp::BSPTextureData_v0>(bsp.view(2), "texture data");
    auto brushModels = records<bsppp::BSPBrushModel_v0>(bsp.view(14), "brush models");
    require(!brushModels.empty(), "The BSP has no world model.");
    struct BrushState { glm::mat4 matrix{1}; glm::vec4 color{1}; bool hidden = false; int renderMode = 0; };
    std::vector<BrushState> brushStates(brushModels.size());
    std::vector<uint32_t> faceModels(faces.size(), 0);
    std::unordered_map<std::string_view, const bsppp::BSPEntityKeyValues*> brushEntities;
    for (const auto& entity : bsp.entities) {
        auto model = entity["model"].getValue();
        if (model.starts_with('*')) { brushEntities.emplace(model, &entity); }
    }
    for (size_t m = 1; m < brushModels.size(); ++m) {
        auto located = brushEntities.find(std::format("*{}", m));
        require(located != brushEntities.end(), "A brush model has no corresponding entity.");
        const auto* found = located->second;
        auto type = std::string((*found)["classname"].getValue());
        auto& state = brushStates[m];
        state.renderMode = static_cast<int>(parseFloat((*found)["rendermode"].getValue()));
        state.hidden = type.starts_with("trigger_") || type == "func_vehicleclip" || type == "func_clip_vphysics" || type == "func_occluder" || type == "func_areaportal" || type == "func_areaportalwindow" || parseFloat((*found)["StartDisabled"].getValue()) != 0;
        bool door = type == "func_door" || type == "func_door_rotating";
        state.hidden = state.hidden || type == "func_traincontrols" || type == "func_smokevolume" || state.renderMode == 10;
        if (type == "func_monitor") { unsupported("Live monitor brush surfaces are not rendered."); state.hidden = true; }
        if (type == "func_lod" && !state.hidden) {
            require(options.experimental, "Distance-dependent func_lod fading is not implemented. Use --experimental to render these brushes without fading.");
            scene.diagnostics["warnings"].push_back("func_lod brushes render without distance fading in experimental captures.");
        }
        require(state.hidden || door || type == "func_tracktrain" || type == "func_train" || type == "func_physbox" || type == "func_rotating" || type == "func_lod" || type == "func_brush" || type == "func_illusionary" || type == "func_wall" || type == "func_button" || type == "func_reflective_glass" || type == "func_breakable", "Unsupported initial brush-entity behavior: {}", type);
        if ((type == "func_physbox" || type == "func_rotating") && !state.hidden) {
            require(options.time == 0 && (*found)["parentname"].getValue().empty(), "'{}' supports only the unparented authored pose at capture time zero.", type);
        }
        require(state.hidden || ((state.renderMode == 0 || state.renderMode == 2 || state.renderMode == 4 || state.renderMode == 5) && parseFloat((*found)["renderfx"].getValue()) == 0), "Brush-entity render effects are not implemented: {} '{}' (mode {}, effect {}).", type, (*found)["targetname"].getValue(), (*found)["rendermode"].getValue(), (*found)["renderfx"].getValue());
        auto origin = parseVector((*found)["origin"].getValue()), angles = parseVector((*found)["angles"].getValue());
        if (type == "func_tracktrain" && !state.hidden) {
            auto pose = initialTrackTrainPose(bsp, *found, options.time); origin = pose.origin; angles = pose.angles;
            scene.diagnostics["tracktrains"].push_back({{"name", std::string((*found)["targetname"].getValue())}, {"origin", {origin.x, origin.y, origin.z}}, {"angles", {angles.x, angles.y, angles.z}}, {"state", "initial_path_pose"}});
        }
        const auto& model = brushModels[m];
        if (type == "func_train" && !state.hidden) {
            auto pose = initialPathTrainPose(bsp, *found, (vec3(model.min) + vec3(model.max)) * 0.5f, options.time); origin = pose.origin; angles = pose.angles;
            scene.diagnostics["path_trains"].push_back({{"name", std::string((*found)["targetname"].getValue())}, {"origin", {origin.x, origin.y, origin.z}}, {"angles", {angles.x, angles.y, angles.z}}, {"state", "initial_path_pose"}});
        }
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
    nodesData = bsp.view(5); planeData = bsp.view(1); leafData = bsp.view(10); leafFaceData = bsp.view(16); visData = bsp.view(4);
    indexLeaves();
    if (options.planarWater && bsp.lumps[36].length) {
        auto waterData = bsp.view(36); Reader water(waterData, "water volumes");
        require(bsp.lumps[36].version == 0 && waterData.size() % 12 == 0, "Unsupported water volume layout.");
        scene.hasWater = true; scene.waterHeight = water.at<float>(0);
        require(std::isfinite(scene.waterHeight), "Invalid water surface height.");
        for (size_t i = 0; i < waterData.size(); i += 12) { require(water.at<float>(i) == scene.waterHeight, "Multiple water surface heights are not implemented."); }
        for (const auto& probe : bsp.probes) { scene.submergedProbes.push_back((Reader(leafData).at<uint32_t>(size_t(leafAt(glm::vec3(probe.origin))) * 32) & 32) != 0); }
        scene.diagnostics["warnings"].push_back("Planar water uses one reflection level and depth refraction; distortion and fog are experimental.");
        scene.diagnostics["warnings"].push_back("Water secondary views omit translucent and additive surfaces. Underwater fog uses the above-water material settings.");
        scene.diagnostics["water"] = {{"mode", "planar"}, {"surface_height", scene.waterHeight}, {"reflection_levels", 1}};
    }
    lightData = bsp.view(options.hdr ? 53 : 8);
    require(!lightData.empty(), "Requested compiled lighting is missing.");
    auto stringsData = bsp.view(43), stringOffsets = bsp.view(44), displacements = bsp.view(26), dispVertices = bsp.view(33);
    auto primitiveData = bsp.view(37), primitiveVertexData = bsp.view(38), primitiveIndexData = bsp.view(39);
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
    std::vector<FaceSet> probeFaces;
    probeFaces.reserve(bsp.probes.size());
    for (const auto& probe : bsp.probes) { probeFaces.push_back(visibleFaces(glm::vec3(probe.origin))); }
    FaceSet referencedFaces;
    Reader referenced(leafFaceData, "leaf face references");
    for (size_t i = 0; i < leafFaceData.size(); i += 2) { referencedFaces.insert(referenced.at<uint16_t>(i)); }
    std::map<std::vector<uint64_t>, uint32_t> visibilityIds;
    std::vector<std::vector<uint64_t>> visibilityMasks;
    std::map<std::tuple<uint32_t, uint32_t, bool, uint32_t>, std::vector<uint32_t>> batches;
    std::map<std::pair<uint32_t, int>, uint32_t> brushMaterials;
    struct NamedMaterial { std::string name; uint32_t index = UINT32_MAX; };
    std::vector<NamedMaterial> texDataMaterials(texData.size());
    std::vector<uint64_t> visible((bsp.probes.size() + 63) / 64);
    std::vector<glm::vec3> polygon;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
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
        std::fill(visible.begin(), visible.end(), uint64_t(0));
        bool anyVisible = false, everywhere = scene.hasWater || faceModels[faceIndex] != 0 || inSkyWorld || !referencedFaces.contains(faceIndex);
        for (size_t p = 0; p < probeFaces.size(); ++p) {
            if (everywhere || probeFaces[p].contains(faceIndex)) { visible[p / 64] |= uint64_t(1) << (p % 64); anyVisible = true; }
        }
        if (!anyVisible) { continue; }
        auto foundVisibility = visibilityIds.find(visible);
        if (foundVisibility == visibilityIds.end()) { foundVisibility = visibilityIds.emplace(visible, static_cast<uint32_t>(visibilityMasks.size())).first; visibilityMasks.push_back(visible); }
        const auto& tex = texData.at(info.textureData);
        auto& named = texDataMaterials[info.textureData];
        if (named.index == UINT32_MAX) {
            uint32_t nameOffset = offsets.at<uint32_t>(size_t(tex.nameStringTableID) * 4);
            named.name = strings.string(nameOffset, stringsData.size() - nameOffset);
            named.index = material(named.name);
        }
        const auto& name = named.name;
        auto materialIndex = named.index;
        if (brush.renderMode != 0) {
            auto key = std::pair(materialIndex, brush.renderMode);
            if (!brushMaterials.contains(key)) {
                auto copy = scene.materials[materialIndex]; copy.name += std::format("@render{}", brush.renderMode);
                if (brush.renderMode == 4) { copy.flags |= AlphaTest; }
                else { copy.flags |= VertexAlpha; copy.translucent = brush.renderMode == 2; copy.additive = brush.renderMode == 5; }
                brushMaterials.emplace(key, static_cast<uint32_t>(scene.materials.size())); scene.materials.push_back(std::move(copy));
            }
            materialIndex = brushMaterials.at(key);
        }
        const auto& mat = scene.materials[materialIndex];
        if (mat.flags & Omitted) { continue; }
        bool bumped = (info.flags & 0x800) != 0;
        require(!(mat.flags & SSBump) || bumped, "SSBump material '{}' has no directional lightmaps.", name);
        auto placement = lightmap(face, bumped);
        polygon.clear();
        for (int i = 0; i < face.numEdges; ++i) {
            int edgeIndex = surfEdges.at(size_t(face.firstEdge) + i).surfEdge;
            const auto& edge = edges.at(static_cast<size_t>(std::abs(int64_t(edgeIndex))));
            polygon.push_back(vec3(points.at(edgeIndex >= 0 ? edge.v0 : edge.v1).position));
        }
        require(polygon.size() >= 3, "Face has fewer than three vertices.");
        auto normal = planes.at<glm::vec3>(size_t(face.planeNum) * 20) * (face.side ? -1.0f : 1.0f);
        if (mat.flags & Water) {
            require(scene.hasWater, "Water faces have no compiled water volume.");
            if (std::abs(normal.z) < 0.999f) { continue; }
            require(faceModels[faceIndex] == 0 && !inSkyWorld && std::abs(polygon.front().z - scene.waterHeight) < 0.1f, "Moving, sloped, and skybox water are not implemented.");
            if (mat.water.above) { scene.waterMaterial = materialIndex; }
        }
        const auto& uvTransform = materialUv(materialIndex);
        auto makeVertex = [&](glm::vec3 position, glm::vec3 uvPosition, float alpha) {
            Vertex vertex;
            vertex.position = glm::vec3(brush.matrix * glm::vec4(position, 1)); vertex.normal = glm::mat3(brush.matrix) * normal;
            auto tangent = glm::normalize(glm::vec3(vec4(info.textureVector1)));
            vertex.tangent = glm::vec4(glm::mat3(brush.matrix) * tangent, glm::dot(glm::cross(normal, tangent), glm::vec3(vec4(info.textureVector2))) < 0 ? -1 : 1);
            glm::vec4 homogeneous(uvPosition, 1);
            vertex.uv = {glm::dot(homogeneous, vec4(info.textureVector1)) / tex.width, glm::dot(homogeneous, vec4(info.textureVector2)) / tex.height};
            vertex.secondaryUV = glm::vec4(uvTransform.secondary(vertex.uv), uvTransform.blend(vertex.uv));
            vertex.uv = uvTransform.base(vertex.uv);
            auto luxel = glm::vec2(glm::dot(homogeneous, vec4(info.lightmapVector1)), glm::dot(homogeneous, vec4(info.lightmapVector2)));
            vertex.lightUV = (luxel - glm::vec2(face.lightmapTextureMinsInLuxels[0], face.lightmapTextureMinsInLuxels[1]) + placement.offset + 0.5f) / 1024.0f;
            vertex.blend = alpha;
            vertex.color = brush.color;
            if ((mat.flags & VertexAlpha) && face.dispInfo >= 0) { vertex.color.a *= alpha; }
            return vertex;
        };
        vertices.clear(); indices.clear();
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
    parallelFor(lightmapTasks.size(), [&](size_t task) { fillLightmap(lightmapTasks[task]); });
    for (auto& [key, indices] : batches) {
        auto [materialIndex, page, sky, visibilityId] = key;
        Draw draw{static_cast<uint32_t>(scene.indices.size()), static_cast<uint32_t>(indices.size()), materialIndex, {page, page ? page + 1 : 0, page ? page + 2 : 0, page ? page + 3 : 0}, sky};
        draw.visibility = visibilityMasks.at(visibilityId);
        scene.draws.push_back(draw);
        scene.indices.insert(scene.indices.end(), indices.begin(), indices.end());
    }
    addProps();
    require(!scene.hasWater || (scene.waterMaterial < scene.materials.size() && (scene.materials[scene.waterMaterial].flags & Water)), "Water volumes require a supported above-water surface material.");
    addSky();
    finishImages();
    for (auto& draw : scene.draws) {
        glm::vec3 minimum(std::numeric_limits<float>::max()), maximum(std::numeric_limits<float>::lowest());
        for (uint32_t i = 0; i < draw.indexCount; ++i) { auto position = scene.vertices.at(scene.indices.at(draw.firstIndex + i)).position; minimum = glm::min(minimum, position); maximum = glm::max(maximum, position); }
        draw.center = (minimum + maximum) * 0.5f; draw.radius = glm::length(maximum - minimum) * 0.5f;
    }
    scene.diagnostics["vertices"] = scene.vertices.size();
    scene.diagnostics["triangles"] = scene.indices.size() / 3;
    scene.diagnostics["draws"] = scene.draws.size();
    scene.diagnostics["water_draws"] = std::count_if(scene.draws.begin(), scene.draws.end(), [&](const auto& draw) { return (scene.materials[draw.material].flags & Water) != 0; });
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
    require((version >= 4 && version <= 6) || version == 10, "Unsupported static-prop layout v{}.", version);
    size_t stride = version == 4 ? 56 : version == 5 ? 60 : version == 6 ? 64 : 72;
    require(data.size() - reader.cursor == size_t(count) * stride, "Static-prop byte count does not match its version.");
    auto ambientIndexData = bsp.view(options.hdr ? 51 : 52), ambientSampleData = bsp.view(options.hdr ? 55 : 56);
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
        uint32_t propFlags = version == 10 ? reader.at<uint32_t>(record + 64) : reader.at<uint8_t>(record + 31);
        require((propFlags & ~0x1ffu) == 0, "Unknown static-prop flags.");
        if (version == 10) { require(reader.at<uint32_t>(record + 68) == 0, "Per-texel static-prop lightmaps are not implemented."); }
        if (propFlags & 4) { continue; }
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
        require(Reader(mdl->bytes, name).at<uint32_t>(8) == Reader(vtx->bytes, name).at<uint32_t>(16) && Reader(mdl->bytes, name).at<uint32_t>(8) == Reader(vvd->bytes, name).at<uint32_t>(8), "Model companion checksums differ: {}", name);
        require(model.open(mdl->bytes, vtx->bytes, vvd->bytes), "Cannot load MDL/VTX/VVD model: {}", name);
        for (const auto& part : model.mdl.bodyParts) { require(part.models.size() <= 1, "Static-prop bodygroups are not supported: {}", name); }
        auto baked = model.processModelData(0);
        require(baked.vertices.size() <= 65535, "Model exceeds SourcePP's current 16-bit baked index range: {}", name);
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
        std::vector<Vertex> propVertices;
        propVertices.reserve(baked.vertices.size());
        for (size_t sourceIndex = 0; sourceIndex < baked.vertices.size(); ++sourceIndex) {
            const auto& source = baked.vertices[sourceIndex];
            Vertex vertex;
            vertex.position = glm::vec3(matrix * glm::vec4(vec3(source.position), 1)); vertex.normal = glm::normalize(normalMatrix * vec3(source.normal));
            vertex.tangent = glm::vec4(normalMatrix * glm::vec3(vec4(source.tangent)), source.tangent[3]);
            vertex.uv = {source.uv[0], source.uv[1]};
            glm::vec3 light(0);
            for (unsigned axis = 0; axis < 3; ++axis) { light += ambient[axis * 2 + (vertex.normal[axis] < 0 ? 1 : 0)] * vertex.normal[axis] * vertex.normal[axis]; }
            if (lighting.baked && bakedColors[sourceIndex].x >= 0) { light = bakedColors[sourceIndex]; }
            vertex.color = glm::vec4(light, 1); propVertices.push_back(vertex);
        }
        int skin = reader.at<int32_t>(record + 32);
        require(skin >= 0 && !model.mdl.skins.empty(), "Static prop {} '{}' has invalid skin metadata.", i, name);
        if (size_t(skin) >= model.mdl.skins.size()) {
            require(options.experimental, "Static prop {} '{}' requests skin {} but the model has {} skin families. Use --experimental to select skin zero.", i, name, skin, model.mdl.skins.size());
            scene.diagnostics["warnings"].push_back(std::format("Static prop {} '{}' requests missing skin {}; experimental rendering uses skin zero.", i, name, skin)); skin = 0;
        }
        for (const auto& mesh : baked.meshes) {
            auto slot = model.mdl.skins.at(skin).at(mesh.materialIndex);
            const auto& materialName = model.mdl.materials.at(slot).name;
            std::string resolved;
            for (const auto& directory : model.mdl.materialDirectories) {
                auto candidate = assetPath(directory + "/" + materialName);
                if (assets.find("materials/" + candidate + ".vmt")) { resolved = std::move(candidate); break; }
            }
            require(!resolved.empty(), "Missing model material '{}' requested by '{}'. Check mounted content and model material directories.", materialName, name);
            auto id = material(resolved); scene.materials[id].flags |= VertexLit;
            const auto& definition = scene.materials[id].definition;
            std::vector<uint32_t> remap(propVertices.size(), UINT32_MAX);
            scene.draws.push_back({static_cast<uint32_t>(scene.indices.size()), static_cast<uint32_t>(mesh.indices.size()), id});
            scene.draws.back().lighting = lightingIndex;
            if (skyArea >= 0) { scene.draws.back().skyWorld = (leavesReader.at<uint16_t>(size_t(leafAt(origin)) * 32 + 6) & 511) == skyArea; }
            for (size_t j = 0; j < mesh.indices.size(); j += 3) {
                require(j + 2 < mesh.indices.size(), "Incomplete model triangle.");
                const auto& uvTransform = materialUv(id);
                for (auto k : {0u, 2u, 1u}) {
                    auto index = mesh.indices[j + k]; require(index < baked.vertices.size(), "Model index exceeds vertex count.");
                    require(!lighting.baked || bakedColors[index].x >= 0, "VHV is missing a rendered model vertex.");
                    if (remap[index] == UINT32_MAX) {
                        auto vertex = propVertices[index];
                        vertex.secondaryUV = glm::vec4(uvTransform.secondary(vertex.uv), uvTransform.blend(vertex.uv));
                        vertex.uv = uvTransform.base(vertex.uv);
                        remap[index] = static_cast<uint32_t>(scene.vertices.size()); scene.vertices.push_back(vertex);
                    }
                    scene.indices.push_back(remap[index]);
                }
            }
        }
    }
    scene.diagnostics["static_props"] = count;
    scene.diagnostics["prop_lighting"] = {{"vhv", bakedProps}, {"ambient_and_world_lights", ambientProps}};
}

void Builder::addSky() {
    auto info = records<bsppp::BSPTextureInfo_v0>(bsp.view(6), "texture information");
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
            require(sky.sunSize >= 0 && sky.duskIntensity >= 0 && sky.duskScale >= 0, "Invalid painted sky sun or dusk parameters.");
            if (sky.sunSize > 0 || sky.duskIntensity > 0) {
                require(options.experimental, "Painted sky sun and dusk shading requires --experimental until native parity is certified.");
                scene.diagnostics["warnings"].push_back("Painted sky sun and dusk shading is experimental; the dusk angular response is approximate.");
            }
            for (const auto& sun : bsp.entities) {
                if (sun["classname"].getValue() != "env_sun") { continue; }
                require(parseFloat(sun["use_angles"].getValue()) != 0, "Position-based env_sun directions are not implemented.");
                auto angles = parseVector(sun["angles"].getValue()); angles.x = parseFloat(sun["pitch"].getValue(), angles.x);
                sky.sunDirection = glm::vec3(transform(glm::vec3(0), angles) * glm::vec4(1,0,0,0));
                scene.diagnostics["warnings"].push_back("The env_sun sprite overlay is not drawn during experimental sky captures.");
                break;
            }
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
size_t pixelBytes(PixelFormat format, uint32_t width, uint32_t height) {
    size_t blocks = size_t((width + 3) / 4) * ((height + 3) / 4), pixels = size_t(width) * height;
    switch (format) {
        case PixelFormat::RGBA8: case PixelFormat::BGRA8: return pixels * 4;
        case PixelFormat::BC1: return blocks * 8;
        case PixelFormat::BC2: case PixelFormat::BC3: return blocks * 16;
        case PixelFormat::RGBA16F: return pixels * 8;
        case PixelFormat::RGBA32F: return pixels * 16;
    }
    throw Error("Unknown texture pixel format.");
}
Image solidImage(std::string name, glm::vec4 color) {
    Image image{std::move(name)};
    storeFloatPixels(image, {color});
    return image;
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
