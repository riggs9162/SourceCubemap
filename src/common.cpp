#include <scm/common.hpp>
#include <Windows.h>
#include <atomic>
#include <fstream>
#include <sstream>
#include <xxhash.h>

namespace scm {
void require(bool condition, std::string_view message) {
    if (!condition) { throw Error(std::string(message)); }
}

Bytes readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(file.good(), std::format("Cannot read {}.", path.string()));
    auto length = file.tellg();
    require(length >= 0 && length <= 2LL * 1024 * 1024 * 1024, "File exceeds the supported 2 GiB limit.");
    Bytes bytes(static_cast<size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), length);
    require(file.good(), std::format("Incomplete read of {}.", path.string()));
    return bytes;
}

void writeFile(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(file.good(), std::format("Cannot create {}.", path.string()));
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    require(file.good(), std::format("Incomplete write of {}.", path.string()));
}

std::string textOf(std::span<const std::byte> bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }
std::string lower(std::string value) {
    for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return value;
}
std::string assetPath(std::string value) {
    value = lower(std::move(value));
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.starts_with("./")) { value.erase(0, 2); }
    require(!value.empty() && value.front() != '/' && value.find(':') == std::string::npos, "Asset paths must be relative.");
    fs::path path(value);
    for (const auto& segment : path) { require(segment != "..", "Asset paths cannot escape a mount."); }
    return path.lexically_normal().generic_string();
}
std::string digest(std::span<const std::byte> bytes) { return std::format("{:016x}", XXH3_64bits(bytes.data(), bytes.size())); }
glm::vec3 parseVector(std::string_view value, glm::vec3 fallback) {
    std::string input(value);
    for (char& c : input) { if (c == '[' || c == ']' || c == '{' || c == '}') { c = ' '; } }
    std::istringstream stream(input);
    glm::vec3 result;
    if (!(stream >> result.x >> result.y >> result.z)) { return fallback; }
    return result;
}
float parseFloat(std::string_view value, float fallback) {
    if (value.empty()) { return fallback; }
    try { return std::stof(std::string(value)); } catch (...) { throw Error(std::format("Invalid number: {}", value)); }
}
std::span<const std::byte> Reader::slice(size_t offset, size_t length) const {
    require(offset <= data.size() && length <= data.size() - offset, std::format("Out-of-bounds range in {}.", label));
    return data.subspan(offset, length);
}
std::string Reader::string(size_t offset, size_t limit) const {
    auto bytes = slice(offset, limit);
    auto end = std::find(bytes.begin(), bytes.end(), std::byte{0});
    require(end != bytes.end(), std::format("Unterminated string in {}.", label));
    return textOf(bytes.first(static_cast<size_t>(end - bytes.begin())));
}
TemporaryFile::TemporaryFile(std::string_view suffix, const fs::path& directory) {
    static std::atomic_uint64_t counter{0};
    fs::create_directories(directory);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        path = directory / std::format("sourcecubemap-{}-{}-{}{}", GetCurrentProcessId(), GetTickCount64(), counter++, suffix);
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); return; }
    }
    throw Error("Cannot reserve a temporary output file.");
}
TemporaryFile::~TemporaryFile() { std::error_code error; fs::remove(path, error); }
double Stopwatch::seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
}
