#include <scm/bsp.hpp>
#include <Windows.h>
#include <set>
#include <bit>
#include <vtfpp/VTF.h>
#include <sourcepp/compression/LZMA.h>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace scm {
void validateVtf(std::span<const std::byte> data, std::string_view name) {
    Reader reader(data, std::string(name));
    require(data.size() >= 64 && reader.at<uint32_t>(0) == 0x00465456 && reader.at<uint32_t>(4) == 7, std::format("Invalid VTF header: {}", name));
    auto minor = reader.at<uint32_t>(8), header = reader.at<uint32_t>(12);
    require(minor <= 5 && header >= (minor >= 2 ? 65u : 64u) && header <= data.size(), std::format("Unsupported or truncated VTF header: {}", name));
    uint32_t width = reader.at<uint16_t>(16), height = reader.at<uint16_t>(18), frames = reader.at<uint16_t>(24), mips = reader.at<uint8_t>(56);
    uint32_t depth = minor >= 2 ? reader.at<uint16_t>(63) : 1;
    require(width && height && frames && depth && mips && width <= 16384 && height <= 16384 && depth <= 2048 && frames <= 4096 && mips <= uint32_t(std::bit_width(std::max({width, height, depth}))), std::format("Invalid VTF image dimensions: {}", name));
    auto format = static_cast<vtfpp::ImageFormat>(reader.at<uint32_t>(52));
    uint32_t faces = (reader.at<uint32_t>(20) & 0x4000) ? (minor < 5 && reader.at<uint16_t>(26) != 0xffff ? 7 : 6) : 1;
    require(faces == 1 || depth == 1, "A VTF cubemap cannot also be a volume texture.");
    uint64_t required = 0;
    for (uint32_t mip = 0; mip < mips; ++mip) {
        required += uint64_t(vtfpp::ImageFormatDetails::getDataLength(format, uint16_t(std::max(1u, width >> mip)), uint16_t(std::max(1u, height >> mip)))) * std::max(1u, depth >> mip) * faces * frames;
    }
    require(required > 0 && required <= (1ull << 30), std::format("Unsupported VTF format or image size: {}", name));
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
    require(imageOffset >= header && imageOffset <= imageEnd && imageEnd <= data.size() && required <= imageEnd - imageOffset, std::format("Truncated VTF image resource: {}", name));
}
std::string Probe::name() const { return std::format("c{}_{}_{}", origin.x, origin.y, origin.z); }

Bsp::Bsp(const fs::path& filename) : path(fs::absolute(filename)), bytes(readFile(path)) {
    Reader header(bytes, path.string());
    require(bytes.size() >= 1036 && header.at<uint32_t>(0) == 0x50534256, "Expected a desktop Source BSP header.");
    require(header.at<uint32_t>(4) == 20, "Only desktop Source BSP v20 is currently supported.");
    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    for (size_t i = 0; i < lumps.size(); ++i) {
        auto& item = lumps[i] = header.at<Lump>(8 + i * 16);
        if (!item.length) { continue; }
        require(item.offset >= 1036, std::format("Lump {} overlaps the BSP header.", i));
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
            uint64_t end = uint64_t(lumps[35].offset) + lumps[35].length;
            if (i + 1 < count) { end = directory.at<uint32_t>(entry + 24); }
            require(flags <= 1 && offset >= uint64_t(lumps[35].offset) + 4 + count * 16 && offset <= end && end <= uint64_t(lumps[35].offset) + lumps[35].length && length <= (1u << 30), "Invalid game-lump directory bounds.");
            if (!signature) { require(i + 1 == count && length == 0, "Invalid game-lump sentinel."); continue; }
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
    std::map<std::string, uint32_t> names;
    for (size_t offset = 0; offset < samples.size(); offset += 16) {
        uint8_t size = sampleReader.at<uint8_t>(offset + 12);
        require(size <= 12, "Cubemap resolution exceeds 2048 pixels.");
        Probe probe{sampleReader.at<glm::ivec3>(offset), size ? (1u << (size - 1)) : 32u};
        if (names.contains(probe.name())) { require(names.at(probe.name()) == probe.resolution, "Duplicate probe origins request different resolutions."); continue; }
        names.emplace(probe.name(), probe.resolution);
        probes.push_back(probe);
    }
    entities = source->getLumpData<bsppp::BSPLump::ENTITIES>();
    auto zip = lump(40);
    if (!zip.empty()) {
        struct ZipReader { void* handle = mz_zip_reader_create(); ~ZipReader() { mz_zip_reader_delete(&handle); } } archive;
        require(archive.handle && zip.size() <= INT32_MAX && mz_zip_reader_open_buffer(archive.handle, reinterpret_cast<const uint8_t*>(zip.data()), static_cast<int32_t>(zip.size()), 0) == MZ_OK, "The embedded ZIP archive cannot be read.");
        uint64_t totalSize = 0; auto status = mz_zip_reader_goto_first_entry(archive.handle);
        for (; status == MZ_OK; status = mz_zip_reader_goto_next_entry(archive.handle)) {
            if (mz_zip_reader_entry_is_dir(archive.handle) == MZ_OK) { continue; }
            mz_zip_file* info = nullptr;
            require(mz_zip_reader_entry_get_info(archive.handle, &info) == MZ_OK && info, "Cannot read an embedded ZIP directory record.");
            auto name = assetPath(info->filename);
            require(!pakNames.contains(name), std::format("Duplicate embedded path: {}", name));
            require(info->uncompressed_size >= 0 && info->uncompressed_size <= (1ll << 30), "An embedded ZIP entry exceeds the 1 GiB memory limit.");
            totalSize += static_cast<uint64_t>(info->uncompressed_size);
            require(totalSize <= (2ull << 30), "The expanded BSP PAK exceeds the 2 GiB memory limit.");
            Bytes payload(static_cast<size_t>(info->uncompressed_size));
            if (payload.empty()) {
                require(mz_zip_reader_entry_open(archive.handle) == MZ_OK && mz_zip_reader_entry_close(archive.handle) == MZ_OK, std::format("Embedded ZIP entry failed validation: {}", name));
            } else {
                require(mz_zip_reader_entry_save_buffer(archive.handle, payload.data(), static_cast<int32_t>(payload.size())) == MZ_OK, std::format("Embedded ZIP entry failed decompression or checksum validation: {}", name));
            }
            pakNames.emplace(name, info->filename); pakData.emplace(std::move(name), std::move(payload));
        }
        require(status == MZ_END_OF_LIST, "The embedded ZIP directory is truncated or invalid.");
    }
}

Bytes Bsp::lump(int index) const {
    require(index >= 0 && index < 64, "Invalid BSP lump index.");
    if (!lumps[index].length) { return {}; }
    auto result = source->getLumpData(static_cast<bsppp::BSPLump>(index));
    require(result.has_value(), std::format("Cannot decode lump {}.", index));
    require(!lumps[index].uncompressed || result->size() == lumps[index].uncompressed, "Decompressed lump size mismatch.");
    return std::move(*result);
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
        size_t end = size_t(lumps[35].offset) + lumps[35].length;
        if (i + 1 < count) { end = directory.at<uint32_t>(entry + 24); }
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
    for (const auto& [name, original] : pakNames) {
        if (!name.ends_with(".vtf")) { continue; }
        auto data = embedded(name);
        validateVtf(*data, name);
        vtfpp::VTF texture(*data);
        require(static_cast<bool>(texture), std::format("Invalid VTF: {}", name));
        if (!(texture.getFlags() & vtfpp::VTF::FLAG_V0_ENVMAP)) { continue; }
        require(texture.getWidth() == texture.getHeight(), std::format("Cubemap is not square: {}", name));
        require(texture.getFaceCount() == 6 || texture.getFaceCount() == 7, std::format("Invalid cubemap face count: {}", name));
        require(texture.getWidth() > 0 && texture.getMipCount() > 0 && texture.getMipCount() <= std::bit_width(texture.getWidth()), std::format("Invalid cubemap dimensions or mip count: {}", name));
        for (uint8_t mip = 0; mip < texture.getMipCount(); ++mip) {
            auto expected = vtfpp::ImageFormatDetails::getDataLength(texture.getFormat(), texture.getWidth(mip), texture.getHeight(mip));
            for (uint16_t frame = 0; frame < texture.getFrameCount(); ++frame) { for (uint8_t face = 0; face < texture.getFaceCount(); ++face) { require(texture.getImageDataRaw(mip, frame, face).size() == expected, std::format("Missing or truncated VTF image data: {}", name)); } }
        }
    }
}

void Bsp::verifyPreserved(const Bsp& original, const std::map<std::string, Bytes>& replacements) const {
    require(source->getMapRevision() == original.source->getMapRevision(), "Map revision changed unexpectedly.");
    for (size_t i = 0; i < 64; ++i) {
        if (i == 40) { continue; }
        require(std::memcmp(&lumps[i], &original.lumps[i], sizeof(Lump)) == 0, std::format("Lump {} metadata changed.", i));
        const auto& l = lumps[i];
        if (l.length == 0) { continue; }
        require(std::equal(bytes.begin() + l.offset, bytes.begin() + l.offset + l.length, original.bytes.begin() + l.offset), std::format("Lump {} payload changed.", i));
    }
    for (const auto& [name, unused] : original.pakNames) {
        if (!replacements.contains(name)) { require(embedded(name) == original.embedded(name), std::format("Unrelated embedded asset changed: {}", name)); }
    }
    for (const auto& [name, expected] : replacements) { auto value = embedded(name); require(value && *value == expected, std::format("Written cubemap differs: {}", name)); }
}

void Bsp::validateDestination(const fs::path& destination, bool overwrite, bool inPlace) const {
    auto output = fs::absolute(destination);
    bool same = fs::exists(output) ? fs::equivalent(path, output) : fs::weakly_canonical(path) == fs::weakly_canonical(output);
    require(!same || inPlace, "Input replacement requires --in-place.");
    require(same || !fs::exists(output) || overwrite, "Output already exists; use --overwrite for a separate output.");
    require(fs::is_directory(output.parent_path()), "The output directory does not exist.");
}

void Bsp::writeCubemaps(const std::map<std::string, Bytes>& textures, const fs::path& destination, bool overwrite, bool inPlace) const {
    validateDestination(destination, overwrite, inPlace);
    auto output = fs::absolute(destination);
    bool same = fs::exists(output) && fs::equivalent(path, output);
    require(!textures.empty(), "No cubemaps were generated; output was not written.");
    TemporaryFile archive(".zip");
    struct ZipWriter { void* handle = mz_zip_writer_create(); ~ZipWriter() { mz_zip_writer_delete(&handle); } } zip;
    auto archivePath = archive.path.u8string();
    require(zip.handle && mz_zip_writer_open_file(zip.handle, reinterpret_cast<const char*>(archivePath.c_str()), 0, 0) == MZ_OK, "Cannot create the output PAK archive.");
    mz_zip_writer_set_compress_method(zip.handle, MZ_COMPRESS_METHOD_STORE);
    auto add = [&](const std::string& name, std::span<const std::byte> data) {
        require(name.size() <= UINT16_MAX && data.size() <= INT32_MAX, "An output ZIP entry exceeds its size limit.");
        mz_zip_file info{}; info.filename = name.c_str(); info.filename_size = static_cast<uint16_t>(name.size());
        info.flag = MZ_ZIP_FLAG_UTF8; info.compression_method = MZ_COMPRESS_METHOD_STORE; info.uncompressed_size = static_cast<int64_t>(data.size());
        require(mz_zip_writer_entry_open(zip.handle, &info) == MZ_OK, std::format("Cannot create embedded entry: {}", name));
        if (!data.empty()) { require(mz_zip_writer_entry_write(zip.handle, data.data(), static_cast<int32_t>(data.size())) == static_cast<int32_t>(data.size()), std::format("Cannot write embedded entry: {}", name)); }
        require(mz_zip_writer_entry_close(zip.handle) == MZ_OK, std::format("Cannot finish embedded entry: {}", name));
    };
    for (const auto& [name, originalName] : pakNames) {
        if (textures.contains(name)) { continue; }
        add(originalName, pakData.at(name));
    }
    for (const auto& [name, data] : textures) { add(assetPath(name), data); }
    require(mz_zip_writer_close(zip.handle) == MZ_OK, "Failed to serialize the output PAK archive.");
    auto packed = readFile(archive.path);
    auto outputBytes = bytes;
    const auto& oldPak = lumps[40];
    size_t oldEnd = size_t(oldPak.offset) + oldPak.length;
    bool terminal = oldPak.length && oldEnd <= bytes.size() && bytes.size() - oldEnd < 4;
    if (terminal) { for (size_t i = oldEnd; i < bytes.size(); ++i) { terminal &= bytes[i] == std::byte{0}; } }
    size_t offset = terminal ? oldPak.offset : ((bytes.size() + 3) & ~size_t(3));
    require(offset + packed.size() <= INT32_MAX, "Output exceeds the BSP signed offset limit.");
    outputBytes.resize(offset + packed.size());
    std::copy(packed.begin(), packed.end(), outputBytes.begin() + offset);
    Lump updated{static_cast<uint32_t>(offset), static_cast<uint32_t>(packed.size()), oldPak.version, 0};
    std::memcpy(outputBytes.data() + 8 + 40 * 16, &updated, sizeof(updated));
    TemporaryFile temporary(".bsp", output.parent_path());
    writeFile(temporary.path, outputBytes);
    Bsp reopened(temporary.path);
    reopened.validateTextures();
    reopened.verifyPreserved(*this, textures);
    HANDLE handle = CreateFileW(temporary.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Cannot open output for flushing.");
    bool flushed = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    require(flushed, "Failed to flush the output BSP.");
    if (same) {
        auto backup = output;
        backup += std::format(".{}.bak", GetTickCount64());
        require(CopyFileW(output.c_str(), backup.c_str(), TRUE) != 0, "Cannot create the original BSP backup.");
    }
    DWORD flags = MOVEFILE_WRITE_THROUGH | ((overwrite || same) ? MOVEFILE_REPLACE_EXISTING : 0);
    require(MoveFileExW(temporary.path.c_str(), output.c_str(), flags) != 0, std::format("Cannot publish the output BSP (Windows error {}).", GetLastError()));
}
}
