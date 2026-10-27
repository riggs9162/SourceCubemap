#include <scm/capture.hpp>
#include <vtfpp/VTF.h>
#include <vtfpp/ImageConversion.h>
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
Cube matchedBorders(const Cube& source) {
    Cube cube = source;
    int size = int(cube.resolution);
    if (size == 1) {
        glm::vec4 average(0);
        for (const auto& face : source.faces) { average += face.front() / 6.0f; }
        for (auto& face : cube.faces) { face.front() = average; }
        return cube;
    }
    for (unsigned face = 0; face < 6; ++face) {
        for (int y = 0; y < size; ++y) { for (int x = 0; x < size; ++x) {
            if (x != 0 && y != 0 && x != size - 1 && y != size - 1) { continue; }
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
                sum += source.faces[c.face][size_t(pixel.y) * size + pixel.x]; ++count;
            }
            cube.faces[face][size_t(y) * size + x] = sum / count;
        } }
    }
    return cube;
}
Cube downsample(const Cube& source) {
    Cube result; result.resolution = std::max(1u, source.resolution / 2);
    for (unsigned face = 0; face < 6; ++face) {
        auto& pixels = result.faces[face]; pixels.resize(size_t(result.resolution) * result.resolution);
        for (uint32_t y = 0; y < result.resolution; ++y) { for (uint32_t x = 0; x < result.resolution; ++x) {
            glm::vec4 sum(0); float weightSum = 0;
            for (int dy = -1; dy <= 2; ++dy) { for (int dx = -1; dx <= 2; ++dx) {
                float weight = float((dx == 0 || dx == 1 ? 3 : 1) * (dy == 0 || dy == 1 ? 3 : 1));
                sum += texel(source, face, int(x * 2) + dx, int(y * 2) + dy) * weight; weightSum += weight;
            } }
            pixels[size_t(y) * result.resolution + x] = sum / weightSum;
        } }
    }
    return matchedBorders(result);
}
float gamma(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1 / 2.4f) - 0.055f;
}
Bytes encodedPixels(std::span<const glm::vec4> source, bool hdr) {
    Bytes bytes(source.size() * (hdr ? sizeof(glm::vec4) : 4));
    if (hdr) {
        auto pixels = std::vector<glm::vec4>(source.begin(), source.end());
        for (auto& pixel : pixels) { pixel = glm::vec4(glm::clamp(glm::vec3(pixel), glm::vec3(0), glm::vec3(65504)), 1); }
        std::memcpy(bytes.data(), pixels.data(), bytes.size());
    } else {
        for (size_t i = 0; i < source.size(); ++i) {
            for (unsigned c = 0; c < 3; ++c) { bytes[i * 4 + c] = std::byte(uint8_t(std::lround(gamma(source[i][c]) * 255))); }
            bytes[i * 4 + 3] = std::byte{255};
        }
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
Bytes encodeCube(const Cube& source, bool hdr) {
    require(source.resolution >= 1 && source.resolution <= 2048 && std::has_single_bit(source.resolution), "Invalid cubemap resolution.");
    for (const auto& face : source.faces) {
        require(face.size() == size_t(source.resolution) * source.resolution, "Incomplete cubemap capture.");
        for (const auto& pixel : face) { for (unsigned c = 0; c < 4; ++c) { require(std::isfinite(pixel[c]), "Cubemap capture contains a non-finite value."); } }
    }
    auto inputFormat = hdr ? vtfpp::ImageFormat::RGBA32323232F : vtfpp::ImageFormat::RGBA8888;
    vtfpp::VTF::CreationOptions settings;
    settings.version = 4; settings.outputFormat = inputFormat; settings.isCubeMap = true;
    settings.flags = vtfpp::VTF::FLAG_V0_CLAMP_S | vtfpp::VTF::FLAG_V0_CLAMP_T;
    settings.computeMips = settings.computeThumbnail = settings.computeReflectivity = settings.computeTransparencyFlags = false;
    auto vtf = vtfpp::VTF::create(inputFormat, uint16_t(source.resolution), uint16_t(source.resolution), settings);
    require(static_cast<bool>(vtf) && vtf.getFaceCount() == 7, "SourcePP did not create a legacy seven-face cubemap.");
    auto mipCount = static_cast<uint8_t>(std::bit_width(source.resolution));
    require(vtf.setMipCount(mipCount), "Cannot allocate cubemap mipmaps.");
    Cube cube = matchedBorders(source);
    glm::dvec3 average(0);
    for (const auto& face : source.faces) { for (const auto& pixel : face) { average += glm::dvec3(pixel); } }
    average /= double(6 * source.resolution * source.resolution);
    vtf.setReflectivity({float(average.x), float(average.y), float(average.z)});
    for (uint8_t mip = 0; mip < mipCount; ++mip) {
        auto sphere = spheremap(cube);
        for (uint8_t face = 0; face < 7; ++face) {
            auto bytes = encodedPixels(face == 6 ? sphere : cube.faces[face], hdr);
            require(vtf.setImage(bytes, inputFormat, uint16_t(cube.resolution), uint16_t(cube.resolution), vtfpp::ImageConversion::ResizeFilter::DEFAULT, mip, 0, face), "Cannot store a cubemap face.");
        }
        if (mip + 1 < mipCount) { cube = downsample(cube); }
    }
    auto outputFormat = hdr ? vtfpp::ImageFormat::RGBA16161616F : vtfpp::ImageFormat::DXT1;
    vtf.setFormat(outputFormat);
    auto result = vtf.bake();
    vtfpp::VTF verified(result);
    require(static_cast<bool>(verified) && verified.getVersion() == 4 && verified.getFaceCount() == 7 && verified.getMipCount() == mipCount && verified.getFormat() == outputFormat, "Encoded VTF failed structural validation.");
    for (uint8_t mip = 0; mip < mipCount; ++mip) { for (uint8_t face = 0; face < 7; ++face) { require(!verified.getImageDataRaw(mip, 0, face).empty(), "Encoded VTF has an empty face or mip."); } }
    return result;
}
void writePreview(const Cube& cube, const fs::path& directory, std::string_view name) {
    fs::create_directories(directory);
    static const std::array<std::string_view, 6> suffix{"px", "nx", "py", "ny", "pz", "nz"};
    for (unsigned face = 0; face < 6; ++face) {
        auto pixels = encodedPixels(cube.faces[face], false);
        auto png = vtfpp::ImageConversion::convertImageDataToFile(pixels, vtfpp::ImageFormat::RGBA8888, uint16_t(cube.resolution), uint16_t(cube.resolution), vtfpp::ImageConversion::FileFormat::PNG);
        require(!png.empty(), "PNG preview encoding failed.");
        writeFile(directory / std::format("{}_{}.png", name, suffix[face]), png);
    }
}
}
