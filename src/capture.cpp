#include <scm/capture.hpp>
#include <vtfpp/VTF.h>
#include <vtfpp/ImageConversion.h>
#include <half.hpp>
#include <immintrin.h>
#include <bit>
#include <cmath>

namespace scm {
namespace {
struct CubeCoordinate { unsigned face; glm::vec2 uv; };
CubeCoordinate coordinate(glm::vec3 direction) {
    auto a = glm::abs(direction);
    require(std::max({a.x, a.y, a.z}) > 0, "Cubemap direction cannot be zero.");
    unsigned face;
    glm::vec2 uv;
    if (a.x >= a.y && a.x >= a.z) {
        face = direction.x >= 0 ? 0 : 1;
        uv = glm::vec2(direction.x >= 0 ? -direction.z : direction.z, -direction.y) / a.x;
    } else if (a.y >= a.z) {
        face = direction.y >= 0 ? 2 : 3;
        uv = glm::vec2(direction.x, direction.y >= 0 ? direction.z : -direction.z) / a.y;
    } else {
        face = direction.z >= 0 ? 4 : 5;
        uv = glm::vec2(direction.z >= 0 ? direction.x : -direction.x, -direction.y) / a.z;
    }
    return {face, uv * 0.5f + 0.5f};
}
glm::vec4 texel(const Cube& cube, unsigned face, int x, int y) {
    auto size = int(cube.resolution);
    if (x < 0 || y < 0 || x >= size || y >= size) {
        auto c = coordinate(cubeDirection(face, (glm::vec2(x, y) + 0.5f) / float(size)));
        face = c.face; x = int(c.uv.x * size); y = int(c.uv.y * size);
    }
    return cube.faces[face][size_t(std::clamp(y, 0, size - 1)) * size + std::clamp(x, 0, size - 1)];
}
void matchBorders(Cube& cube) {
    int size = int(cube.resolution);
    if (size == 1) {
        glm::vec4 average(0);
        for (const auto& face : cube.faces) { average += face.front() / 6.0f; }
        for (auto& face : cube.faces) { face.front() = average; }
        return;
    }
    auto forEachBorder = [&](auto&& visit) {
        for (unsigned face = 0; face < 6; ++face) {
            for (int y = 0; y < size; ++y) {
                int step = y == 0 || y == size - 1 ? 1 : size - 1;
                for (int x = 0; x < size; x += step) { visit(face, x, y); }
            }
        }
    };
    std::vector<glm::vec4> matched; matched.reserve(size_t(size) * 24);
    forEachBorder([&](unsigned face, int x, int y) {
        auto uv = (glm::vec2(x, y) + 0.5f) / float(size);
        if (x == 0) { uv.x = 0; } else if (x == size - 1) { uv.x = 1; }
        if (y == 0) { uv.y = 0; } else if (y == size - 1) { uv.y = 1; }
        auto direction = cubeDirection(face, uv);
        glm::vec4 sum(0); float count = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (std::abs(std::abs(direction[axis]) - 1.0f) > 0.0001f) { continue; }
            auto biased = direction; biased[axis] *= 1.0001f;
            auto c = coordinate(biased);
            auto pixel = glm::clamp(glm::ivec2(c.uv * float(size)), glm::ivec2(0), glm::ivec2(size - 1));
            sum += cube.faces[c.face][size_t(pixel.y) * size + pixel.x]; ++count;
        }
        matched.push_back(sum / count);
    });
    size_t next = 0;
    forEachBorder([&](unsigned face, int x, int y) { cube.faces[face][size_t(y) * size + x] = matched[next++]; });
}
Cube downsample(const Cube& source) {
    Cube result; result.resolution = std::max(1u, source.resolution / 2);
    static const std::array<__m128, 16> weights = [] {
        std::array<__m128, 16> values{};
        for (int dy = 0; dy < 4; ++dy) { for (int dx = 0; dx < 4; ++dx) { values[size_t(dy) * 4 + dx] = _mm_set1_ps(float((dx == 1 || dx == 2 ? 3 : 1) * (dy == 1 || dy == 2 ? 3 : 1))); } }
        return values;
    }();
    int size = int(source.resolution);
    for (unsigned face = 0; face < 6; ++face) {
        auto& pixels = result.faces[face]; pixels.resize(size_t(result.resolution) * result.resolution);
        const auto* input = source.faces[face].data();
        for (uint32_t y = 0; y < result.resolution; ++y) { for (uint32_t x = 0; x < result.resolution; ++x) {
            int left = int(x * 2) - 1, top = int(y * 2) - 1;
            if (left >= 0 && top >= 0 && left + 3 < size && top + 3 < size) {
                __m128 sum = _mm_setzero_ps();
                for (int dy = 0; dy < 4; ++dy) {
                    const auto* row = input + size_t(top + dy) * size + left;
                    for (int dx = 0; dx < 4; ++dx) { sum = _mm_add_ps(sum, _mm_mul_ps(_mm_loadu_ps(&row[dx].x), weights[size_t(dy) * 4 + dx])); }
                }
                _mm_storeu_ps(&pixels[size_t(y) * result.resolution + x].x, _mm_div_ps(sum, _mm_set1_ps(64)));
                continue;
            }
            glm::vec4 sum(0); float weightSum = 0;
            for (int dy = -1; dy <= 2; ++dy) { for (int dx = -1; dx <= 2; ++dx) {
                float weight = float((dx == 0 || dx == 1 ? 3 : 1) * (dy == 0 || dy == 1 ? 3 : 1));
                sum += texel(source, face, int(x * 2) + dx, int(y * 2) + dy) * weight; weightSum += weight;
            } }
            pixels[size_t(y) * result.resolution + x] = sum / weightSum;
        } }
    }
    matchBorders(result);
    return result;
}
float gamma(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1 / 2.4f) - 0.055f;
}
uint8_t gammaByte(float value) { return uint8_t(std::lround(gamma(value) * 255)); }
/// Converts linear channels to sRGB bytes with the exact rounding of gammaByte, replacing a power per channel with byte thresholds found once.
class GammaTable {
public:
    GammaTable() {
        for (unsigned level = 1; level < 256; ++level) {
            auto low = std::bit_cast<uint32_t>(0.0f), high = std::bit_cast<uint32_t>(1.0f);
            while (low < high) {
                auto middle = low + (high - low) / 2;
                if (gammaByte(std::bit_cast<float>(middle)) >= level) { high = middle; } else { low = middle + 1; }
            }
            thresholds[level] = std::bit_cast<float>(low);
        }
        thresholds[256] = std::numeric_limits<float>::infinity();
        unsigned level = 0;
        for (size_t bucket = 0; bucket < buckets.size(); ++bucket) {
            while (float(bucket) / 4096.0f >= thresholds[level + 1]) { ++level; }
            buckets[bucket] = uint8_t(level);
        }
    }
    uint8_t operator()(float value) const {
        if (!(value > 0)) { return 0; }
        if (value >= 1) { return 255; }
        unsigned level = buckets[size_t(value * 4096.0f)];
        while (value >= thresholds[level + 1]) { ++level; }
        return uint8_t(level);
    }
private:
    std::array<float, 257> thresholds{};
    std::array<uint8_t, 4097> buckets{};
};
Bytes rgbaPixels(std::span<const glm::vec4> source) {
    static const GammaTable encode;
    Bytes bytes(source.size() * 4);
    auto* output = reinterpret_cast<uint8_t*>(bytes.data());
    for (const auto& pixel : source) {
        output[0] = encode(pixel.r); output[1] = encode(pixel.g); output[2] = encode(pixel.b); output[3] = 255;
        output += 4;
    }
    return bytes;
}
std::vector<glm::vec4> spheremap(const Cube& cube) {
    std::vector<glm::vec4> pixels(size_t(cube.resolution) * cube.resolution);
    for (uint32_t y = 0; y < cube.resolution; ++y) { for (uint32_t x = 0; x < cube.resolution; ++x) {
        auto xy = (glm::vec2(x, y) + 0.5f) * (2.0f / cube.resolution) - 1.0f;
        float radius = glm::dot(xy, xy);
        if (radius > 1) { xy /= std::sqrt(radius); radius = 1; }
        glm::vec3 normal(xy.x, -xy.y, std::sqrt(1 - radius));
        auto direction = glm::reflect(glm::vec3(0, 0, -1), normal);
        pixels[size_t(y) * cube.resolution + x] = sampleCube(cube, direction);
    } }
    return pixels;
}
void compressFace(std::span<const glm::vec4> pixels, uint32_t size, std::byte* destination) {
    auto rgba = rgbaPixels(pixels);
    if (size < 4) { rgba = vtfpp::ImageConversion::padImageData(rgba, vtfpp::ImageFormat::RGBA8888, uint16_t(size), uint16_t(4 - size), uint16_t(size), uint16_t(4 - size)); size = 4; }
    compressDxt1(rgba, size, destination);
}
void packFace(std::span<const glm::vec4> pixels, std::byte* destination) {
    auto* output = reinterpret_cast<half_float::half*>(destination);
    for (const auto& pixel : pixels) {
        auto color = glm::clamp(glm::vec3(pixel), glm::vec3(0), glm::vec3(65504));
        output[0] = half_float::half(color.r); output[1] = half_float::half(color.g); output[2] = half_float::half(color.b); output[3] = half_float::half(1.0f);
        output += 4;
    }
}
glm::vec3 averageColor(const Cube& source) {
    glm::dvec3 average(0);
    for (const auto& face : source.faces) { for (const auto& pixel : face) { average += glm::dvec3(pixel); } }
    return glm::vec3(average / double(6 * source.resolution * source.resolution));
}
/// Encodes cubes below the DXT block size through SourcePP, which pads them to a whole block.
Bytes encodeTinyCube(Cube cube) {
    auto reflectivity = averageColor(cube);
    vtfpp::VTF::CreationOptions settings;
    settings.version = 4; settings.outputFormat = vtfpp::ImageFormat::RGBA8888; settings.isCubeMap = true;
    settings.flags = vtfpp::VTF::FLAG_V0_CLAMP_S | vtfpp::VTF::FLAG_V0_CLAMP_T;
    settings.computeMips = settings.computeThumbnail = settings.computeReflectivity = settings.computeTransparencyFlags = false;
    auto vtf = vtfpp::VTF::create(vtfpp::ImageFormat::RGBA8888, uint16_t(cube.resolution), uint16_t(cube.resolution), settings);
    require(static_cast<bool>(vtf) && vtf.getFaceCount() == 7, "SourcePP did not create a legacy seven-face cubemap.");
    auto mipCount = static_cast<uint8_t>(std::bit_width(cube.resolution));
    require(vtf.setMipCount(mipCount), "Cannot allocate cubemap mipmaps.");
    matchBorders(cube);
    vtf.setReflectivity({reflectivity.x, reflectivity.y, reflectivity.z});
    for (uint8_t mip = 0; mip < mipCount; ++mip) {
        auto sphere = spheremap(cube);
        for (uint8_t face = 0; face < 7; ++face) {
            auto bytes = rgbaPixels(face == 6 ? std::span<const glm::vec4>(sphere) : std::span<const glm::vec4>(cube.faces[face]));
            require(vtf.setImage(bytes, vtfpp::ImageFormat::RGBA8888, uint16_t(cube.resolution), uint16_t(cube.resolution), vtfpp::ImageConversion::ResizeFilter::DEFAULT, mip, 0, face), "Cannot store a cubemap face.");
        }
        if (mip + 1 < mipCount) { cube = downsample(cube); }
    }
    vtf.setFormat(vtfpp::ImageFormat::DXT1);
    return vtf.bake();
}
/// Writes the VTF 7.4 layout directly: an 88-byte header, one image resource, then each mip from smallest to largest with its seven faces.
Bytes encodeFullCube(Cube cube, bool hdr) {
    auto reflectivity = averageColor(cube);
    uint32_t mipCount = std::bit_width(cube.resolution);
    auto faceBytes = [&](uint32_t mip) { auto size = std::max(1u, cube.resolution >> mip); auto blocks = (size + 3) / 4; return hdr ? size_t(size) * size * 8 : size_t(blocks) * blocks * 8; };
    constexpr size_t headerBytes = 88;
    std::vector<size_t> offsets(mipCount);
    size_t totalBytes = headerBytes;
    for (uint32_t mip = mipCount; mip-- > 0;) { offsets[mip] = totalBytes; totalBytes += faceBytes(mip) * 7; }
    Bytes result(totalBytes);
    auto put = [&](size_t offset, auto value) { std::memcpy(result.data() + offset, &value, sizeof(value)); };
    uint32_t flags = vtfpp::VTF::FLAG_V0_CLAMP_S | vtfpp::VTF::FLAG_V0_CLAMP_T | vtfpp::VTF::FLAG_V0_NO_LOD | vtfpp::VTF::FLAG_V0_ENVMAP | (mipCount == 1 ? vtfpp::VTF::FLAG_V0_NO_MIP : 0);
    put(0, uint32_t(0x00465456)); put(4, uint32_t(7)); put(8, uint32_t(4)); put(12, uint32_t(headerBytes));
    put(16, uint16_t(cube.resolution)); put(18, uint16_t(cube.resolution)); put(20, flags); put(24, uint16_t(1));
    put(32, reflectivity); put(48, 1.0f); put(52, static_cast<uint32_t>(hdr ? vtfpp::ImageFormat::RGBA16161616F : vtfpp::ImageFormat::DXT1));
    put(56, uint8_t(mipCount)); put(57, static_cast<uint32_t>(vtfpp::ImageFormat::EMPTY)); put(63, uint16_t(1)); put(68, uint32_t(1));
    put(80, uint32_t(0x30)); put(84, uint32_t(headerBytes));
    matchBorders(cube);
    std::vector<Cube> chain; chain.reserve(mipCount); chain.push_back(std::move(cube));
    for (uint32_t mip = 1; mip < mipCount; ++mip) { chain.push_back(downsample(chain.back())); }
    parallelFor(size_t(mipCount) * 7, [&](size_t item) {
        auto mip = uint32_t(item / 7); auto face = unsigned(item % 7);
        const auto& level = chain[mip];
        auto sphere = face == 6 ? spheremap(level) : std::vector<glm::vec4>{};
        std::span<const glm::vec4> pixels = face == 6 ? std::span<const glm::vec4>(sphere) : std::span<const glm::vec4>(level.faces[face]);
        auto* destination = result.data() + offsets[mip] + faceBytes(mip) * face;
        if (hdr) { packFace(pixels, destination); } else { compressFace(pixels, level.resolution, destination); }
    });
    return result;
}
}

glm::vec3 cubeDirection(unsigned face, glm::vec2 uv) {
    float u = uv.x * 2 - 1, v = uv.y * 2 - 1;
    switch (face) {
        case 0: return {1, -v, -u};
        case 1: return {-1, -v, u};
        case 2: return {u, 1, v};
        case 3: return {u, -1, -v};
        case 4: return {u, -v, 1};
        case 5: return {-u, -v, -1};
        default: throw Error("Invalid cubemap face.");
    }
}
glm::vec4 sampleCube(const Cube& cube, glm::vec3 direction) {
    auto c = coordinate(direction);
    auto pixel = c.uv * float(cube.resolution) - 0.5f;
    auto base = glm::ivec2(glm::floor(pixel)); auto fraction = glm::fract(pixel);
    return glm::mix(glm::mix(texel(cube, c.face, base.x, base.y), texel(cube, c.face, base.x + 1, base.y), fraction.x), glm::mix(texel(cube, c.face, base.x, base.y + 1), texel(cube, c.face, base.x + 1, base.y + 1), fraction.x), fraction.y);
}
Bytes encodeCube(Cube&& source, bool hdr) {
    require(source.resolution >= 1 && source.resolution <= 2048 && std::has_single_bit(source.resolution), "Invalid cubemap resolution.");
    for (const auto& face : source.faces) {
        require(face.size() == size_t(source.resolution) * source.resolution, "Incomplete cubemap capture.");
        for (const auto& pixel : face) { for (unsigned c = 0; c < 4; ++c) { require(std::isfinite(pixel[c]), "Cubemap capture contains a non-finite value."); } }
    }
    auto result = !hdr && source.resolution < 4 ? encodeTinyCube(std::move(source)) : encodeFullCube(std::move(source), hdr);
    validateVtf(result, "generated cubemap");
    return result;
}
Bytes encodeCube(const Cube& source, bool hdr) { return encodeCube(Cube(source), hdr); }
Cube decodeCube(std::span<const std::byte> texture, bool hdr) {
    validateVtf(texture, "cached cubemap preview");
    Bytes bytes(texture.begin(), texture.end()); vtfpp::VTF vtf(bytes);
    require(vtf && vtf.getFaceCount() >= 6 && vtf.getWidth() == vtf.getHeight(), "Cached preview requires a square cubemap.");
    Cube cube; cube.resolution = vtf.getWidth();
    for (uint8_t face = 0; face < 6; ++face) {
        auto data = vtf.getImageDataAs(vtfpp::ImageFormat::RGBA32323232F, 0, 0, face);
        auto& pixels = cube.faces[face]; pixels.resize(size_t(cube.resolution) * cube.resolution);
        require(data.size() == pixels.size() * sizeof(glm::vec4), "Cached cubemap face could not be decoded.");
        std::memcpy(pixels.data(), data.data(), data.size());
        if (!hdr) { for (auto& pixel : pixels) { for (unsigned channel = 0; channel < 3; ++channel) { float value = pixel[channel]; pixel[channel] = value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f); } } }
    }
    return cube;
}
void writePreview(const Cube& cube, const fs::path& directory, std::string_view name) {
    fs::create_directories(directory);
    static const std::array<std::string_view, 6> suffix{"px", "nx", "py", "ny", "pz", "nz"};
    for (unsigned face = 0; face < 6; ++face) {
        auto pixels = rgbaPixels(cube.faces[face]);
        auto png = vtfpp::ImageConversion::convertImageDataToFile(pixels, vtfpp::ImageFormat::RGBA8888, uint16_t(cube.resolution), uint16_t(cube.resolution), vtfpp::ImageConversion::FileFormat::PNG);
        require(!png.empty(), "PNG preview encoding failed.");
        writeFile(directory / std::format("{}_{}.png", name, suffix[face]), png);
    }
}
}
