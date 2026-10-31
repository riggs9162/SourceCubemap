#pragma once

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace scm {
namespace fs = std::filesystem;
using Bytes = std::vector<std::byte>;
using Json = nlohmann::json;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view message);
/// Throws Error with the message when a validated condition does not hold.
inline void require(bool condition, std::string_view message) {
    if (!condition) [[unlikely]] { fail(message); }
}
/// Throws Error with a formatted message; arguments are formatted only on failure so hot validation stays free.
template<class... Args> inline void require(bool condition, std::format_string<Args...> message, Args&&... arguments) {
    if (!condition) [[unlikely]] { fail(std::vformat(message.get(), std::make_format_args(arguments...))); }
}
Bytes readFile(const fs::path& path);
void writeFile(const fs::path& path, std::span<const std::byte> bytes);
/// Writes several byte ranges to one file in order, without joining them in memory first.
void writeFileParts(const fs::path& path, std::initializer_list<std::span<const std::byte>> parts);
std::string textOf(std::span<const std::byte> bytes);
std::string lower(std::string value);
std::string assetPath(std::string value);
std::string digest(std::span<const std::byte> bytes);
/// Runs body(0..count-1) across CPU threads and rethrows the first failure after every item has finished.
void parallelFor(size_t count, const std::function<void(size_t)>& body);
glm::vec3 parseVector(std::string_view value, glm::vec3 fallback = {});
float parseFloat(std::string_view value, float fallback = 0.0f);

/// Reads little-endian fields after checking the complete requested byte range.
class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes, std::string context = "binary data") : data(bytes), label(std::move(context)) {}
    template<class T> T at(size_t offset) const {
        static_assert(std::is_trivially_copyable_v<T>);
        require(offset <= data.size() && sizeof(T) <= data.size() - offset, "Truncated {} at byte {}.", label, offset);
        T value;
        std::memcpy(&value, data.data() + offset, sizeof(T));
        return value;
    }
    template<class T> T take() { auto value = at<T>(cursor); cursor += sizeof(T); return value; }
    std::span<const std::byte> slice(size_t offset, size_t length) const;
    std::string string(size_t offset, size_t limit) const;
    size_t cursor = 0;
    std::span<const std::byte> data;
    std::string label;
};

class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view suffix, const fs::path& directory = fs::temp_directory_path());
    ~TemporaryFile();
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    fs::path path;
};

class Stopwatch {
public:
    double seconds() const;
private:
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
}
