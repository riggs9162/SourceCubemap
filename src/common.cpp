#include <scm/common.hpp>
#include <Windows.h>
#include <atomic>
#include <execution>
#include <fstream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <xxhash.h>

namespace scm {
void fail(std::string_view message) { throw Error(std::string(message)); }

Bytes readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(file.good(), "Cannot read {}.", path.string());
    auto length = file.tellg();
    require(length >= 0 && length <= 2LL * 1024 * 1024 * 1024, "File exceeds the supported 2 GiB limit.");
    Bytes bytes(static_cast<size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), length);
    require(file.good(), "Incomplete read of {}.", path.string());
    return bytes;
}

void writeFile(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(file.good(), "Cannot create {}.", path.string());
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    require(file.good(), "Incomplete write of {}.", path.string());
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
void parallelFor(size_t count, const std::function<void(size_t)>& body) {
    std::vector<size_t> items(count); std::iota(items.begin(), items.end(), size_t(0));
    std::exception_ptr failure; std::mutex guard;
    std::for_each(std::execution::par, items.begin(), items.end(), [&](size_t item) {
        try { body(item); }
        catch (...) { std::scoped_lock lock(guard); if (!failure) { failure = std::current_exception(); } }
    });
    if (failure) { std::rethrow_exception(failure); }
}
glm::vec3 parseVector(std::string_view value, glm::vec3 fallback) {
    if (value.empty()) { return fallback; }
    std::string input(value);
    for (char& c : input) { if (c == '[' || c == ']' || c == '{' || c == '}') { c = ' '; } }
    std::istringstream stream(input);
    glm::vec3 result;
    require(static_cast<bool>(stream >> result.x), "Invalid vector: {}", value);
    stream >> std::ws;
    if (stream.eof()) { result.y = result.z = result.x; }
    else { require(static_cast<bool>(stream >> result.y >> result.z), "Invalid vector: {}", value); stream >> std::ws; }
    require(stream.eof() && std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z), "Invalid or nonfinite vector: {}", value);
    return result;
}
float parseFloat(std::string_view value, float fallback) {
    if (value.empty()) { return fallback; }
    std::istringstream stream{std::string(value)};
    float result = 0;
    require(static_cast<bool>(stream >> result) && std::isfinite(result), "Invalid or nonfinite number: {}", value);
    stream >> std::ws;
    require(stream.eof(), "Unexpected text after number: {}", value);
    return result;
}
std::span<const std::byte> Reader::slice(size_t offset, size_t length) const {
    require(offset <= data.size() && length <= data.size() - offset, "Out-of-bounds range in {}.", label);
    return data.subspan(offset, length);
}
std::string Reader::string(size_t offset, size_t limit) const {
    auto bytes = slice(offset, limit);
    auto end = std::find(bytes.begin(), bytes.end(), std::byte{0});
    require(end != bytes.end(), "Unterminated string in {}.", label);
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
