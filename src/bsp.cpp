#include <scm/bsp.hpp>
#include <Windows.h>
#include <atomic>
#include <future>
#include <thread>
#include <set>
#include <bit>
#include <vtfpp/VTF.h>
#include <sourcepp/compression/LZMA.h>
#include <mz.h>
#include <mz_crypt.h>
#include <mz_strm.h>
#include <mz_strm_mem.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace scm {
namespace {
/// Returns where a game-lump entry ends. Repacked maps close the directory with a terminator that stores offset zero, so that entry ends at its container.
uint64_t gameLumpEnd(const Reader& directory, size_t entry, uint32_t index, uint32_t count, uint64_t container) {
    if (index + 1 >= count) { return container; }
    auto next = directory.at<uint32_t>(entry + 24);
    return next ? next : container;
}
/// One independent reader over the embedded ZIP, so several threads can decompress entries from the same memory.
struct ZipView {
    void* stream = mz_stream_mem_create();
    void* zip = mz_zip_create();
    explicit ZipView(std::span<const std::byte> data) {
        require(stream && zip, "Cannot allocate an embedded ZIP reader.");
        mz_stream_mem_open(stream, nullptr, MZ_OPEN_MODE_READ);
        mz_stream_mem_set_buffer(stream, const_cast<std::byte*>(data.data()), static_cast<int32_t>(data.size()));
        require(mz_zip_open(zip, stream, MZ_OPEN_MODE_READ) == MZ_OK, "The embedded ZIP archive cannot be read.");
    }
    ~ZipView() { mz_zip_close(zip); mz_zip_delete(&zip); mz_stream_mem_delete(&stream); }
    ZipView(const ZipView&) = delete;
    ZipView& operator=(const ZipView&) = delete;
};
/// Decompresses one entry to its recorded size and checks its CRC here, because Valve's repacker writes LZMA entries without an end marker that the decoder could detect.
bool readZipEntry(void* zip, int64_t position, uint32_t crc, Bytes& payload) {
    if (mz_zip_goto_entry(zip, position) != MZ_OK || mz_zip_entry_read_open(zip, 0, nullptr) != MZ_OK) { return false; }
    size_t offset = 0;
    while (offset < payload.size()) {
        auto read = mz_zip_entry_read(zip, payload.data() + offset, static_cast<int32_t>(payload.size() - offset));
        if (read <= 0) { mz_zip_entry_close(zip); return false; }
        offset += static_cast<size_t>(read);
    }
    mz_zip_entry_close(zip);
    return payload.empty() || mz_crypt_crc32_update(0, reinterpret_cast<const uint8_t*>(payload.data()), static_cast<int32_t>(payload.size())) == crc;
}
}
void validateVtf(std::span<const std::byte> data, std::string_view name) {
    Reader reader(data, name);
    require(data.size() >= 64 && reader.at<uint32_t>(0) == 0x00465456 && reader.at<uint32_t>(4) == 7, "Invalid VTF header: {}", name);
    auto minor = reader.at<uint32_t>(8), header = reader.at<uint32_t>(12);
    require(minor <= 5 && header >= (minor >= 2 ? 65u : 64u) && header <= data.size(), "Unsupported or truncated VTF header: {}", name);
    uint32_t width = reader.at<uint16_t>(16), height = reader.at<uint16_t>(18), frames = reader.at<uint16_t>(24), mips = reader.at<uint8_t>(56);
    uint32_t depth = minor >= 2 ? reader.at<uint16_t>(63) : 1;
    require(width && height && frames && depth && mips && width <= 16384 && height <= 16384 && depth <= 2048 && frames <= 4096 && mips <= uint32_t(std::bit_width(std::max({width, height, depth}))), "Invalid VTF image dimensions: {}", name);
    auto format = static_cast<vtfpp::ImageFormat>(reader.at<uint32_t>(52));
    uint32_t faces = (reader.at<uint32_t>(20) & 0x4000) ? (minor < 5 && reader.at<uint16_t>(26) != 0xffff ? 7 : 6) : 1;
    require(faces == 1 || depth == 1, "A VTF cubemap cannot also be a volume texture.");
    uint64_t required = 0;
    for (uint32_t mip = 0; mip < mips; ++mip) {
        required += uint64_t(vtfpp::ImageFormatDetails::getDataLength(format, uint16_t(std::max(1u, width >> mip)), uint16_t(std::max(1u, height >> mip)))) * std::max(1u, depth >> mip) * faces * frames;
    }
    require(required > 0 && required <= (1ull << 30), "Unsupported VTF format or image size: {}", name);
    uint64_t imageOffset = header, imageEnd = data.size();
    if (minor >= 3) {
        require(header >= 80, "VTF resource header is truncated.");
        auto resources = reader.at<uint32_t>(68);
        require(resources > 0 && resources <= 32 && 80 + resources * 8 <= header, "Invalid VTF resource table.");
        bool found = false;
        for (uint32_t i = 0; i < resources; ++i) {
            auto tag = reader.at<uint32_t>(80 + i * 8);
            if ((tag & 0xffffff) != 0x30) { continue; }
            require(!found && !(tag & 0xff000000), "Invalid or duplicated VTF image resource.");
            imageOffset = reader.at<uint32_t>(84 + i * 8); found = true;
        }
        require(found, "VTF has no high-resolution image resource.");
        for (uint32_t i = 0; i < resources; ++i) {
            auto tag = reader.at<uint32_t>(80 + i * 8), offset = reader.at<uint32_t>(84 + i * 8);
            require((tag & 0x02000000) || (offset >= header && offset < data.size()), "A VTF resource points outside its payload.");
            if (!(tag & 0x02000000) && offset > imageOffset) { imageEnd = std::min(imageEnd, uint64_t(offset)); }
        }
    } else {
        auto thumbnailFormat = static_cast<vtfpp::ImageFormat>(reader.at<uint32_t>(57));
        imageOffset += vtfpp::ImageFormatDetails::getDataLength(thumbnailFormat, reader.at<uint8_t>(61), reader.at<uint8_t>(62));
    }
    require(imageOffset >= header && imageOffset <= imageEnd && imageEnd <= data.size() && required <= imageEnd - imageOffset, "Truncated VTF image resource: {}", name);
}
std::string Probe::name() const { return std::format("c{}_{}_{}", origin.x, origin.y, origin.z); }

Bsp::Bsp(const fs::path& filename) : path(fs::absolute(filename)), bytes(readFile(path)) {
    auto label = path.string();
    Reader header(bytes, label);
    require(bytes.size() >= 1036 && header.at<uint32_t>(0) == 0x50534256, "Expected a desktop Source BSP header.");
    require(header.at<uint32_t>(4) == 20, "Only desktop Source BSP v20 is currently supported.");
    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    for (size_t i = 0; i < lumps.size(); ++i) {
        auto& item = lumps[i] = header.at<Lump>(8 + i * 16);
        if (!item.length) { continue; }
        require(item.offset >= 1036, "Lump {} overlaps the BSP header.", i);
        header.slice(item.offset, item.length);
        require(item.uncompressed <= 1024u * 1024u * 1024u, "Decompressed lump exceeds the 1 GiB limit.");
        if (item.uncompressed) {
            Reader compressed(header.slice(item.offset, item.length), "compressed BSP lump");
            require(item.length >= 17 && compressed.at<uint32_t>(0) == 0x414d5a4c && compressed.at<uint32_t>(4) == item.uncompressed && compressed.at<uint32_t>(8) <= item.length - 17, "Invalid Valve LZMA envelope or decompressed size.");
        }
        intervals.emplace_back(item.offset, uint64_t(item.offset) + item.length);
    }
    std::sort(intervals.begin(), intervals.end());
    for (size_t i = 1; i < intervals.size(); ++i) { require(intervals[i].first >= intervals[i - 1].second, "BSP lumps overlap."); }
    if (lumps[35].length) {
        require(lumps[35].uncompressed == 0, "An outer-compressed game-lump directory is not supported.");
        Reader directory(header.slice(lumps[35].offset, lumps[35].length), "game-lump directory");
        auto count = directory.at<uint32_t>(0);
        require(count <= 1024, "The game-lump directory exceeds the supported entry limit.");
        directory.slice(4, size_t(count) * 16);
        std::set<uint32_t> signatures;
        for (uint32_t i = 0; i < count; ++i) {
            size_t entry = 4 + size_t(i) * 16;
            auto signature = directory.at<uint32_t>(entry);
            auto flags = directory.at<uint16_t>(entry + 4);
            auto offset = directory.at<uint32_t>(entry + 8), length = directory.at<uint32_t>(entry + 12);
            if (!signature) { require(i + 1 == count && length == 0, "Invalid game-lump sentinel."); continue; }
            uint64_t end = gameLumpEnd(directory, entry, i, count, uint64_t(lumps[35].offset) + lumps[35].length);
            require(flags <= 1 && offset >= uint64_t(lumps[35].offset) + 4 + count * 16 && offset <= end && end <= uint64_t(lumps[35].offset) + lumps[35].length && length <= (1u << 30), "Invalid game-lump directory bounds.");
            require(signatures.insert(signature).second, "Duplicate game-lump signature.");
            if (!flags) { require(length <= end - offset, "Game lumps overlap."); }
            else {
                Reader compressed(header.slice(offset, end - offset), "compressed game lump");
                require(end - offset >= 17 && compressed.at<uint32_t>(0) == 0x414d5a4c && compressed.at<uint32_t>(4) == length && compressed.at<uint32_t>(8) <= end - offset - 17, "Invalid compressed game-lump envelope.");
            }
        }
    }
    source = std::make_unique<bsppp::BSP>(path.string(), false);
    require(static_cast<bool>(*source), "SourcePP could not parse the BSP.");
    auto samples = lump(42);
    require(lumps[42].version == 0 && samples.size() % 16 == 0, "Unsupported cubemap sample layout.");
    Reader sampleReader(samples, "cubemap samples");
    std::map<std::string, size_t> names;
    for (size_t offset = 0; offset < samples.size(); offset += 16) {
        uint8_t size = sampleReader.at<uint8_t>(offset + 12);
        require(size <= 12, "Cubemap resolution exceeds 2048 pixels.");
        Probe probe{sampleReader.at<glm::ivec3>(offset), size ? (1u << (size - 1)) : 32u};
        auto [existing, added] = names.try_emplace(probe.name(), probes.size());
        if (added) { probes.push_back(probe); }
        else { probes[existing->second].resolution = std::max(probes[existing->second].resolution, probe.resolution); }
    }
    entities = source->getLumpData<bsppp::BSPLump::ENTITIES>();
    auto zip = view(40);
    if (!zip.empty()) {
        struct PendingEntry { const std::string* name; int64_t position; uint32_t crc; Bytes* payload; };
        std::vector<PendingEntry> pending;
        require(zip.size() <= INT32_MAX, "The embedded ZIP archive cannot be read.");
        ZipView directory(zip);
        uint64_t totalSize = 0; auto status = mz_zip_goto_first_entry(directory.zip);
        for (; status == MZ_OK; status = mz_zip_goto_next_entry(directory.zip)) {
            if (mz_zip_entry_is_dir(directory.zip) == MZ_OK) { continue; }
            mz_zip_file* info = nullptr;
            require(mz_zip_entry_get_info(directory.zip, &info) == MZ_OK && info, "Cannot read an embedded ZIP directory record.");
            auto name = assetPath(info->filename);
            require(!pakNames.contains(name), "Duplicate embedded path: {}", name);
            require(info->uncompressed_size >= 0 && info->uncompressed_size <= (1ll << 30), "An embedded ZIP entry exceeds the 1 GiB memory limit.");
            totalSize += static_cast<uint64_t>(info->uncompressed_size);
            require(totalSize <= (2ull << 30), "The expanded BSP PAK exceeds the 2 GiB memory limit.");
            pakNames.emplace(name, info->filename);
            auto [entry, inserted] = pakData.emplace(std::move(name), Bytes(static_cast<size_t>(info->uncompressed_size)));
            pending.push_back({&entry->first, mz_zip_get_entry(directory.zip), info->crc, &entry->second});
        }
        require(status == MZ_END_OF_LIST, "The embedded ZIP directory is truncated or invalid.");
        std::atomic_size_t next = 0;
        parallelFor(std::min<size_t>(pending.size(), std::max(1u, std::thread::hardware_concurrency())), [&](size_t) {
            ZipView worker(zip);
            for (size_t i = next++; i < pending.size(); i = next++) {
                const auto& entry = pending[i];
                require(readZipEntry(worker.zip, entry.position, entry.crc, *entry.payload), "Embedded ZIP entry failed decompression or checksum validation: {}", *entry.name);
            }
        });
    }
}

Bytes Bsp::lump(int index) const {
    require(index >= 0 && index < 64, "Invalid BSP lump index.");
    const auto& item = lumps[index];
    if (!item.length) { return {}; }
    auto stored = std::span<const std::byte>(bytes).subspan(item.offset, item.length);
    if (!item.uncompressed) { return Bytes(stored.begin(), stored.end()); }
    auto result = sourcepp::compression::decompressValveLZMA(stored);
    require(result.has_value(), "Cannot decode lump {}.", index);
    require(result->size() == item.uncompressed, "Decompressed lump size mismatch.");
    return std::move(*result);
}

std::span<const std::byte> Bsp::view(int index) const {
    require(index >= 0 && index < 64, "Invalid BSP lump index.");
    const auto& item = lumps[index];
    if (!item.length) { return {}; }
    if (!item.uncompressed) { return std::span<const std::byte>(bytes).subspan(item.offset, item.length); }
    decodeLumps({&index, 1});
    std::scoped_lock lock(decodedLock);
    return decoded.at(index);
}

void Bsp::decodeLumps(std::span<const int> indices) const {
    std::vector<int> pending;
    {
        std::scoped_lock lock(decodedLock);
        for (int index : indices) { if (lumps[index].length && lumps[index].uncompressed && !decoded.contains(index)) { pending.push_back(index); } }
    }
    std::vector<Bytes> results(pending.size());
    parallelFor(pending.size(), [&](size_t i) {
        const auto& item = lumps[pending[i]];
        auto result = sourcepp::compression::decompressValveLZMA(std::span<const std::byte>(bytes).subspan(item.offset, item.length));
        require(result.has_value(), "Cannot decode lump {}.", pending[i]);
        require(result->size() == item.uncompressed, "Decompressed lump size mismatch.");
        results[i] = std::move(*result);
    });
    std::scoped_lock lock(decodedLock);
    for (size_t i = 0; i < pending.size(); ++i) { decoded.emplace(pending[i], std::move(results[i])); }
}

std::optional<Bytes> Bsp::embedded(std::string_view name) const {
    auto key = assetPath(std::string(name));
    auto found = pakData.find(key);
    if (found == pakData.end()) { return {}; }
    return found->second;
}

std::optional<GameLump> Bsp::gameLump(uint32_t signature) const {
    if (!lumps[35].length) { return {}; }
    require(lumps[35].uncompressed == 0, "An outer-compressed game-lump directory is not supported.");
    Reader file(bytes, "BSP game-lump payload"), directory(file.slice(lumps[35].offset, lumps[35].length), "game-lump directory");
    auto count = directory.at<uint32_t>(0);
    directory.slice(4, size_t(count) * 16);
    std::optional<GameLump> result;
    for (uint32_t i = 0; i < count; ++i) {
        size_t entry = 4 + size_t(i) * 16;
        if (directory.at<uint32_t>(entry) != signature) { continue; }
        require(!result, "Duplicate game-lump signature.");
        auto flags = directory.at<uint16_t>(entry + 4), version = directory.at<uint16_t>(entry + 6);
        auto offset = directory.at<uint32_t>(entry + 8), length = directory.at<uint32_t>(entry + 12);
        require(flags <= 1 && length <= (1u << 30), "Unsupported game-lump flags or size.");
        size_t end = static_cast<size_t>(gameLumpEnd(directory, entry, i, count, uint64_t(lumps[35].offset) + lumps[35].length));
        require(offset >= lumps[35].offset + 4 + size_t(count) * 16 && offset <= end && end <= size_t(lumps[35].offset) + lumps[35].length, "Game-lump offsets are outside their container.");
        auto payload = file.slice(offset, flags ? end - offset : length);
        if (flags) {
            auto decoded = sourcepp::compression::decompressValveLZMA(payload);
            require(decoded && decoded->size() == length, "Game-lump decompression failed.");
            result = GameLump{version, std::move(*decoded)};
        } else { require(length <= end - offset, "Game lumps overlap."); result = GameLump{version, Bytes(payload.begin(), payload.end())}; }
    }
    return result;
}

Json Bsp::inspect() const {
    Json result{{"path", path.string()}, {"version", 20}, {"bytes", bytes.size()}, {"map_revision", source->getMapRevision()}, {"pak_entries", pakNames.size()}, {"has_ldr", lumps[8].length != 0}, {"has_hdr", lumps[53].length != 0}, {"probes", Json::array()}, {"lumps", Json::array()}};
    for (const auto& p : probes) { result["probes"].push_back({{"name", p.name()}, {"origin", {p.origin.x, p.origin.y, p.origin.z}}, {"resolution", p.resolution}}); }
    for (size_t i = 0; i < lumps.size(); ++i) { const auto& l = lumps[i]; if (l.length) { result["lumps"].push_back({{"id", i}, {"offset", l.offset}, {"bytes", l.length}, {"version", l.version}, {"uncompressed_bytes", l.uncompressed}}); } }
    if (auto props = gameLump(0x73707270)) {
        Reader reader(props->bytes, "static props"); auto count = reader.take<uint32_t>(); Json models = Json::array();
        for (uint32_t i = 0; i < count; ++i) { models.push_back(reader.string(reader.cursor, 128)); reader.cursor += 128; }
        auto leaves = reader.take<uint32_t>(); reader.slice(reader.cursor, size_t(leaves) * 2); reader.cursor += size_t(leaves) * 2;
        result["static_props"] = {{"version", props->version}, {"models", models}, {"instances", reader.take<uint32_t>()}};
    }
    return result;
}

void Bsp::validateTextures() const {
    std::vector<const std::pair<const std::string, Bytes>*> textures;
    for (const auto& entry : pakData) { if (entry.first.ends_with(".vtf")) { textures.push_back(&entry); } }
    parallelFor(textures.size(), [&](size_t index) {
        const auto& [name, data] = *textures[index];
        validateVtf(data, name);
        vtfpp::VTF texture(data);
        require(static_cast<bool>(texture), "Invalid VTF: {}", name);
        if (!(texture.getFlags() & vtfpp::VTF::FLAG_V0_ENVMAP)) { return; }
        require(texture.getWidth() == texture.getHeight(), "Cubemap is not square: {}", name);
        require(texture.getFaceCount() == 6 || texture.getFaceCount() == 7, "Invalid cubemap face count: {}", name);
        require(texture.getWidth() > 0 && texture.getMipCount() > 0 && texture.getMipCount() <= std::bit_width(texture.getWidth()), "Invalid cubemap dimensions or mip count: {}", name);
        for (uint8_t mip = 0; mip < texture.getMipCount(); ++mip) {
            auto expected = vtfpp::ImageFormatDetails::getDataLength(texture.getFormat(), texture.getWidth(mip), texture.getHeight(mip));
            for (uint16_t frame = 0; frame < texture.getFrameCount(); ++frame) { for (uint8_t face = 0; face < texture.getFaceCount(); ++face) { require(texture.getImageDataRaw(mip, frame, face).size() == expected, "Missing or truncated VTF image data: {}", name); } }
        }
    });
}

void Bsp::verifyPreserved(const Bsp& original, const TextureMap& replacements) const {
    require(source->getMapRevision() == original.source->getMapRevision(), "Map revision changed unexpectedly.");
    for (size_t i = 0; i < 64; ++i) {
        if (i == 40) { continue; }
        require(std::memcmp(&lumps[i], &original.lumps[i], sizeof(Lump)) == 0, "Lump {} metadata changed.", i);
        const auto& l = lumps[i];
        if (l.length == 0) { continue; }
        require(std::equal(bytes.begin() + l.offset, bytes.begin() + l.offset + l.length, original.bytes.begin() + l.offset), "Lump {} payload changed.", i);
    }
    for (const auto& [name, payload] : original.pakData) {
        if (replacements.contains(name)) { continue; }
        auto found = pakData.find(name);
        require(found != pakData.end() && found->second == payload, "Unrelated embedded asset changed: {}", name);
    }
    for (const auto& [name, expected] : replacements) {
        auto found = pakData.find(assetPath(name));
        require(found != pakData.end() && std::ranges::equal(found->second, expected), "Written cubemap differs: {}", name);
    }
}

void Bsp::validateDestination(const fs::path& destination, bool overwrite, bool inPlace) const {
    auto output = fs::absolute(destination);
    bool same = fs::exists(output) ? fs::equivalent(path, output) : fs::weakly_canonical(path) == fs::weakly_canonical(output);
    require(!same || inPlace, "Input replacement requires --in-place.");
    require(same || !fs::exists(output) || overwrite, "Output already exists; use --overwrite for a separate output.");
    require(fs::is_directory(output.parent_path()), "The output directory does not exist.");
}

std::string Bsp::writeCubemaps(const TextureMap& textures, const fs::path& destination, bool overwrite, bool inPlace, bool backup) const {
    validateDestination(destination, overwrite, inPlace);
    auto output = fs::absolute(destination);
    bool same = fs::exists(output) && fs::equivalent(path, output);
    require(!textures.empty(), "No cubemaps were generated; output was not written.");
    size_t estimate = 1u << 20;
    for (const auto& [name, data] : pakData) { estimate += data.size() + name.size() * 2 + 128; }
    for (const auto& [name, data] : textures) { estimate += data.size() + name.size() * 2 + 128; }
    require(estimate <= INT32_MAX, "Output exceeds the BSP signed offset limit.");
    struct MemoryStream { void* handle = mz_stream_mem_create(); ~MemoryStream() { mz_stream_mem_delete(&handle); } } memory;
    require(memory.handle != nullptr, "Cannot create the output PAK archive.");
    mz_stream_mem_set_grow_size(memory.handle, static_cast<int32_t>(estimate));
    require(mz_stream_mem_open(memory.handle, nullptr, MZ_OPEN_MODE_CREATE) == MZ_OK, "Cannot create the output PAK archive.");
    struct ZipWriter { void* handle = mz_zip_writer_create(); ~ZipWriter() { mz_zip_writer_delete(&handle); } } zip;
    require(zip.handle && mz_zip_writer_open(zip.handle, memory.handle, 0) == MZ_OK, "Cannot create the output PAK archive.");
    mz_zip_writer_set_compress_method(zip.handle, MZ_COMPRESS_METHOD_STORE);
    auto add = [&](const std::string& name, std::span<const std::byte> data) {
        require(name.size() <= UINT16_MAX && data.size() <= INT32_MAX, "An output ZIP entry exceeds its size limit.");
        mz_zip_file info{}; info.filename = name.c_str(); info.filename_size = static_cast<uint16_t>(name.size());
        info.flag = MZ_ZIP_FLAG_UTF8; info.compression_method = MZ_COMPRESS_METHOD_STORE; info.uncompressed_size = static_cast<int64_t>(data.size());
        require(mz_zip_writer_entry_open(zip.handle, &info) == MZ_OK, "Cannot create embedded entry: {}", name);
        if (!data.empty()) { require(mz_zip_writer_entry_write(zip.handle, data.data(), static_cast<int32_t>(data.size())) == static_cast<int32_t>(data.size()), "Cannot write embedded entry: {}", name); }
        require(mz_zip_writer_entry_close(zip.handle) == MZ_OK, "Cannot finish embedded entry: {}", name);
    };
    for (const auto& [name, originalName] : pakNames) {
        if (textures.contains(name)) { continue; }
        add(originalName, pakData.at(name));
    }
    for (const auto& [name, data] : textures) { add(assetPath(name), data); }
    require(mz_zip_writer_close(zip.handle) == MZ_OK, "Failed to serialize the output PAK archive.");
    const void* packedData = nullptr; int32_t packedLength = 0;
    require(mz_stream_mem_get_buffer(memory.handle, &packedData) == MZ_OK, "Failed to serialize the output PAK archive.");
    mz_stream_mem_get_buffer_length(memory.handle, &packedLength);
    std::span<const std::byte> packed(static_cast<const std::byte*>(packedData), static_cast<size_t>(packedLength));
    const auto& oldPak = lumps[40];
    size_t oldEnd = size_t(oldPak.offset) + oldPak.length;
    bool terminal = oldPak.length && oldEnd <= bytes.size() && bytes.size() - oldEnd < 4;
    if (terminal) { for (size_t i = oldEnd; i < bytes.size(); ++i) { terminal &= bytes[i] == std::byte{0}; } }
    size_t offset = terminal ? oldPak.offset : ((bytes.size() + 3) & ~size_t(3));
    require(offset + packed.size() <= INT32_MAX, "Output exceeds the BSP signed offset limit.");
    Lump updated{static_cast<uint32_t>(offset), static_cast<uint32_t>(packed.size()), oldPak.version, 0};
    constexpr size_t pakRecord = 8 + 40 * 16;
    auto retained = std::span<const std::byte>(bytes).first(std::min(offset, bytes.size()));
    Bytes padding(offset - retained.size());
    TemporaryFile temporary(".bsp", output.parent_path());
    writeFileParts(temporary.path, {retained.first(pakRecord), std::as_bytes(std::span(&updated, 1)), retained.subspan(pakRecord + sizeof(updated)), padding, packed});
    auto flushing = std::async(std::launch::async, [&] {
        HANDLE handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "Cannot open output for flushing.");
        bool flushed = FlushFileBuffers(handle) != 0;
        CloseHandle(handle);
        require(flushed, "Failed to flush the output BSP.");
    });
    Bsp reopened(temporary.path);
    reopened.validateTextures();
    reopened.verifyPreserved(*this, textures);
    auto validated = digest(reopened.bytes);
    flushing.get();
    if (same && backup) {
        auto copy = output;
        copy += std::format(".{}.bak", GetTickCount64());
        require(CopyFileW(output.c_str(), copy.c_str(), TRUE) != 0, "Cannot create the original BSP backup.");
    }
    DWORD flags = MOVEFILE_WRITE_THROUGH | ((overwrite || same) ? MOVEFILE_REPLACE_EXISTING : 0);
    require(MoveFileExW(temporary.path.c_str(), output.c_str(), flags) != 0, "Cannot publish the output BSP (Windows error {}).", GetLastError());
    return validated;
}
}
