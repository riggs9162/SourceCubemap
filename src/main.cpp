#include <scm/capture.hpp>
#include <scm/cache.hpp>
#include <vtfpp/VTF.h>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <Windows.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <iostream>
#include <set>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

namespace {
struct Options {
    std::string input, game, output, report, asset, cache, profile = "gmod", hdr = "auto", quality = "standard", device, previews, water = "strict";
    std::vector<std::string> mounts;
    bool overwrite = false, inPlace = false, verbose = false, experimental = false, noCache = false, incremental = false;
    bool progressJson = false;
    std::string cancelEvent;
    double captureTime = 0;
    unsigned runs = 3;
    unsigned cacheLimitMb = 2048;
};
void progress(const Options& options, std::string stage, unsigned current = 0, unsigned total = 0) {
    if (options.progressJson) { std::cout << scm::Json{{"event", "progress"}, {"stage", stage}, {"current", current}, {"total", total}}.dump() << '\n' << std::flush; }
}
class Cancellation {
public:
    explicit Cancellation(const std::string& name) {
        if (!name.empty()) { event = OpenEventA(SYNCHRONIZE, FALSE, name.c_str()); scm::require(event != nullptr, "The requested cancellation event is unavailable."); }
    }
    ~Cancellation() { if (event) { CloseHandle(event); } }
    void check() const { scm::require(!event || WaitForSingleObject(event, 0) != WAIT_OBJECT_0, "Cancelled by user. No BSP was written."); }
private:
    HANDLE event = nullptr;
};
bool aliases(const scm::fs::path& first, const scm::fs::path& second) {
    if (scm::fs::exists(first) && scm::fs::exists(second)) { return scm::fs::equivalent(first, second); }
    return scm::lower(scm::fs::weakly_canonical(scm::fs::absolute(first)).generic_string()) == scm::lower(scm::fs::weakly_canonical(scm::fs::absolute(second)).generic_string());
}
scm::fs::path shaderDirectory() {
    std::wstring path(32768, L'\0');
    auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    scm::require(count > 0 && count < path.size(), "Cannot locate executable shaders.");
    path.resize(count);
    return scm::fs::path(path).parent_path() / "shaders";
}
std::set<std::string> namespaces(const scm::Bsp& bsp) {
    std::set<std::string> names;
    if (auto metadata = bsp.embedded("sourcecubemap/build.json")) {
        auto record = scm::Json::parse(scm::textOf(*metadata));
        scm::require(record.value("schema", 0) == 1, "Unsupported embedded SourceCubemap metadata version.");
        auto name = record.at("map_namespace").get<std::string>();
        scm::require(!name.empty() && name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..", "Invalid original cubemap namespace in build metadata.");
        names.insert(scm::lower(name)); return names;
    }
    for (const auto& [name, original] : bsp.pakNames) {
        if (!name.starts_with("materials/maps/")) { continue; }
        auto end = name.find('/', 15);
        if (end != std::string::npos) { names.insert(name.substr(15, end - 15)); }
    }
    if (names.empty()) { names.insert(scm::lower(bsp.path.stem().string())); }
    return names;
}
/// Picks the namespace recorded in build metadata: the map's own name when embedded, otherwise the one holding the most embedded files.
std::string primaryNamespace(const scm::Bsp& bsp, const std::set<std::string>& names) {
    auto own = scm::lower(bsp.path.stem().string());
    if (names.contains(own)) { return own; }
    std::string best = *names.begin();
    size_t bestCount = 0;
    for (const auto& name : names) {
        auto prefix = "materials/maps/" + name + "/";
        auto count = static_cast<size_t>(std::count_if(bsp.pakNames.begin(), bsp.pakNames.end(), [&](const auto& entry) { return entry.first.starts_with(prefix); }));
        if (count > bestCount) { best = name; bestCount = count; }
    }
    return best;
}
bool needsDefaultSky(const scm::Bsp& bsp, scm::Assets& assets, bool hdr, const std::set<std::string>& mapNames, bool experimental) {
    std::set<std::string> probes, defaults;
    bool needed = false;
    for (const auto& mapName : mapNames) {
        auto prefix = "maps/" + mapName + "/";
        for (const auto& probe : bsp.probes) { probes.insert(prefix + probe.name()); }
        defaults.insert(prefix + "cubemapdefault");
        needed = needed || bsp.pakNames.contains("materials/" + prefix + "cubemapdefault.vtf") || bsp.pakNames.contains("materials/" + prefix + "cubemapdefault.hdr.vtf");
    }
    auto names = bsp.lump(43); scm::Reader strings(names, "compiled material names");
    for (size_t offset = 0; offset < names.size();) {
        auto name = strings.string(offset, names.size() - offset); offset += name.size() + 1;
        scm::MaterialDefinition material;
        try { material = scm::readMaterial(assets, name, hdr); }
        catch (const scm::Error&) { continue; }
        auto binding = material.value("$envmap");
        if (binding.empty()) { continue; }
        binding = scm::assetPath(binding);
        if (binding.starts_with("materials/")) { binding.erase(0, 10); }
        if (binding.ends_with(".vtf")) { binding.resize(binding.size() - 4); }
        if (binding.ends_with(".hdr")) { binding.resize(binding.size() - 4); }
        if (!binding.starts_with("maps/")) { continue; }
        if (defaults.contains(binding)) { needed = true; }
        else if (!probes.contains(binding)) {
            scm::require(experimental, "Material '{}' references cubemap '{}' without a matching BSP sample. The binding was preserved; output was not written.", name, binding);
            spdlog::warn("Material '{}' references cubemap '{}' without a matching BSP sample; the binding was preserved.", name, binding);
        }
    }
    return needed;
}
scm::SceneOptions sceneSettings(const Options& options, bool hdr, bool defaultSky) {
    scm::SceneOptions settings{hdr, options.captureTime, options.cache, !options.noCache, options.experimental, defaultSky};
    settings.planarWater = options.water == "planar";
    return settings;
}
/// Encodes captures on background threads. Submission waits while too many pixels are queued, and the first failure surfaces on the next submission or in finish.
class EncoderPool {
public:
    EncoderPool() {
        for (unsigned i = 0, count = std::max(2u, std::thread::hardware_concurrency() / 2); i < count; ++i) { workers.emplace_back([this] { run(); }); }
    }
    ~EncoderPool() {
        { std::scoped_lock lock(guard); stopping = true; }
        changed.notify_all();
        for (auto& worker : workers) { worker.join(); }
    }
    void submit(size_t bytes, std::move_only_function<void()> job) {
        std::unique_lock lock(guard);
        changed.wait(lock, [&] { return failure || pendingBytes == 0 || pendingBytes + bytes <= maximumPendingBytes; });
        if (failure) { std::rethrow_exception(failure); }
        pendingBytes += bytes;
        queue.emplace_back(bytes, std::move(job));
        changed.notify_all();
    }
    void finish() {
        std::unique_lock lock(guard);
        changed.wait(lock, [&] { return failure || pendingBytes == 0; });
        if (failure) { std::rethrow_exception(failure); }
    }
private:
    static constexpr size_t maximumPendingBytes = 4ull << 30;
    void run() {
        std::unique_lock lock(guard);
        while (true) {
            changed.wait(lock, [&] { return stopping || !queue.empty(); });
            if (stopping) { return; }
            auto [bytes, job] = std::move(queue.front()); queue.pop_front();
            bool abandoned = failure != nullptr;
            lock.unlock();
            std::exception_ptr error;
            if (!abandoned) {
                try { job(); }
                catch (...) { error = std::current_exception(); }
            }
            job = nullptr;
            lock.lock();
            pendingBytes -= bytes;
            if (error && !failure) { failure = error; }
            changed.notify_all();
        }
    }
    std::mutex guard;
    std::condition_variable changed;
    std::deque<std::pair<size_t, std::move_only_function<void()>>> queue;
    std::vector<std::thread> workers;
    std::exception_ptr failure;
    size_t pendingBytes = 0;
    bool stopping = false;
};
/// Keeps each encoded texture once and lists it under every output namespace.
class TextureStore {
public:
    explicit TextureStore(const std::set<std::string>& namespaces) : names(namespaces) {}
    void add(const std::string& name, scm::Bytes bytes) {
        std::scoped_lock lock(guard);
        const auto& stored = storage.emplace_back(std::move(bytes));
        for (const auto& map : names) { textures.emplace("materials/maps/" + map + "/" + name + ".vtf", stored); }
    }
    void clear() { textures.clear(); storage.clear(); }
    scm::TextureMap textures;
private:
    const std::set<std::string>& names;
    std::mutex guard;
    std::deque<scm::Bytes> storage;
};
struct CaptureRequest { scm::Probe probe; bool skyOnly; std::string name; };
std::vector<CaptureRequest> captureRequests(const scm::Bsp& bsp, bool hdr, bool defaultSky) {
    std::vector<CaptureRequest> requests;
    std::string suffix = hdr ? ".hdr" : "";
    for (const auto& probe : bsp.probes) { requests.push_back({probe, false, probe.name() + suffix}); }
    if (defaultSky) { requests.push_back({scm::Probe{glm::ivec3(0), 32}, true, "cubemapdefault" + suffix}); }
    return requests;
}
scm::fs::path captureFile(const Options& options, std::string_view cacheKey, std::string_view name) { return scm::fs::path(options.cache) / "captures" / cacheKey / (std::string(name) + ".bin"); }
std::optional<scm::Bytes> readCapture(const scm::fs::path& file, const scm::Probe& probe) {
    if (!scm::fs::exists(file)) { return {}; }
    try {
        auto cached = scm::readFile(file);
        scm::require(cached.size() >= 16, "Truncated capture cache.");
        auto payload = std::span<const std::byte>(cached).subspan(16);
        scm::require(scm::digest(payload) == scm::textOf(std::span<const std::byte>(cached).first(16)), "Capture cache checksum mismatch.");
        scm::validateVtf(payload, file.string());
        scm::Bytes bytes(payload.begin(), payload.end()); vtfpp::VTF vtf(bytes);
        scm::require(static_cast<bool>(vtf) && vtf.getFaceCount() == 7 && vtf.getWidth() == probe.resolution && vtf.getHeight() == probe.resolution, "Invalid cached cubemap.");
        return bytes;
    } catch (const std::exception& error) { spdlog::warn("Ignoring capture cache: {}", error.what()); return {}; }
}
/// Identifies the installed graphics adapters and drivers without starting Vulkan, so unchanged builds can be recognized cheaply.
std::string adapterIdentity() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { return {}; }
    std::string identity;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 description{}; LARGE_INTEGER driver{};
        if (FAILED(adapter->GetDesc1(&description))) { return {}; }
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
        identity += std::format("{:x}:{:x}:{:x}:{:x};", description.VendorId, description.DeviceId, description.Revision, driver.QuadPart);
    }
    return identity;
}
scm::fs::path buildManifest(const Options& options, const scm::Bsp& bsp, const scm::Assets& assets, std::string_view shaders) {
    scm::Json key{{"schema", 1}, {"version", SCM_VERSION}, {"bsp", scm::digest(bsp.bytes)}, {"mounts", assets.report().at("mounts")}, {"shaders", shaders}, {"quality", options.quality}, {"hdr", options.hdr}, {"time", options.captureTime}, {"experimental", options.experimental}, {"water", options.water}, {"device", options.device}, {"profile", options.profile}};
    auto text = key.dump();
    return scm::fs::path(options.cache) / "captures" / ("build-" + scm::digest(std::as_bytes(std::span(text.data(), text.size())))) / "manifest.json";
}
struct ReusedBuild { scm::Json scenes = scm::Json::array(), renderer; size_t captures = 0; };
/// Restores a previous build without Vulkan or scene loading when its manifest, adapters, dependencies, and every capture still match.
std::optional<ReusedBuild> reuseBuild(const Options& options, const scm::Bsp& bsp, scm::Assets& assets, const std::vector<bool>& variants, const scm::fs::path& file, std::string_view adapters, TextureStore& store) {
    if (adapters.empty() || !scm::fs::exists(file)) { return {}; }
    try {
        auto manifest = scm::Json::parse(scm::textOf(scm::readFile(file)));
        const auto& recorded = manifest.at("variants");
        if (manifest.at("adapters").get<std::string>() != adapters || recorded.size() != variants.size()) { return {}; }
        for (size_t i = 0; i < variants.size(); ++i) {
            if (recorded[i].at("hdr").get<bool>() != variants[i] || recorded[i].at("scene_key").get<std::string>() != scm::sceneCacheKey(bsp, assets, sceneSettings(options, variants[i], recorded[i].at("default_sky")))) { return {}; }
        }
        const auto& dependencies = recorded.back().at("assets");
        for (const auto& name : dependencies.at("unresolved_lookups")) { if (assets.find(name.get<std::string>())) { return {}; } }
        for (const auto& dependency : dependencies.at("resolved")) {
            auto current = assets.find(dependency.at("path").get<std::string>());
            if (!current || current->origin != dependency.at("source").get<std::string>() || scm::digest(current->bytes) != dependency.at("hash").get<std::string>()) { return {}; }
        }
        ReusedBuild result; result.renderer = manifest.at("renderer");
        for (auto& [key, value] : result.renderer.items()) { if (value.is_number() && key != "driver_version") { value = 0; } }
        std::atomic_bool complete = true;
        for (const auto& variant : recorded) {
            bool hdr = variant.at("hdr");
            auto requests = captureRequests(bsp, hdr, variant.at("default_sky"));
            auto cacheKey = variant.at("cache_key").get<std::string>();
            scm::parallelFor(requests.size(), [&](size_t index) {
                const auto& request = requests[index];
                auto bytes = readCapture(captureFile(options, cacheKey, request.name), request.probe);
                if (!bytes) { complete = false; return; }
                if (!options.previews.empty()) { scm::writePreview(scm::decodeCube(*bytes, hdr), options.previews, request.name); }
                store.add(request.name, std::move(*bytes));
            });
            if (!complete) { store.clear(); return {}; }
            result.captures += requests.size();
            auto geometry = variant.at("geometry"); geometry["scene_cache_hit"] = true;
            result.scenes.push_back({{"hdr", hdr}, {"geometry", std::move(geometry)}, {"assets", variant.at("assets")}});
        }
        return result;
    } catch (const std::exception& error) { spdlog::warn("Ignoring build manifest: {}", error.what()); store.clear(); return {}; }
}
struct LoadedScene { bool defaultSky = false; scm::Scene scene; std::string sceneKey; scm::Json assets; double seconds = 0; };
scm::Json build(const Options& options, const scm::fs::path& output) {
    scm::require(std::isfinite(options.captureTime) && options.captureTime >= 0, "Capture time must be a finite nonnegative number.");
    Cancellation cancellation(options.cancelEvent); cancellation.check(); progress(options, "parsing");
    scm::Stopwatch total, parsing;
    struct CacheFlush { ~CacheFlush() { scm::flushCacheWrites(); } } cacheFlush;
    double startupSeconds = 0;
    auto startRenderer = [&] {
        return std::async(std::launch::async, [&] {
            scm::Stopwatch startup;
            auto renderer = std::make_unique<scm::Renderer>(scm::RenderOptions{options.device, options.quality, shaderDirectory(), options.noCache ? scm::fs::path{} : scm::fs::path(options.cache)});
            startupSeconds = startup.seconds();
            return renderer;
        });
    };
    bool reuseAllowed = options.incremental && !options.noCache;
    std::unique_ptr<scm::Renderer> renderer;
    std::future<std::unique_ptr<scm::Renderer>> starting;
    if (!reuseAllowed) { starting = startRenderer(); }
    auto identifying = std::async(std::launch::async, [&] { return options.noCache ? std::string{} : adapterIdentity(); });
    scm::Bsp bsp(options.input);
    bsp.validateDestination(output, options.overwrite, options.inPlace);
    scm::require(!bsp.probes.empty(), "The map contains no cubemap samples.");
    auto boundNamespaces = namespaces(bsp), names = boundNamespaces;
    auto mapNamespace = primaryNamespace(bsp, boundNamespaces);
    names.insert(scm::lower(output.stem().string()));
    scm::Json times{{"parsing", parsing.seconds()}};
    std::vector<bool> variants;
    if ((options.hdr == "auto" || options.hdr == "ldr" || options.hdr == "both") && bsp.lumps[8].length) { variants.push_back(false); }
    if ((options.hdr == "auto" || options.hdr == "hdr" || options.hdr == "both") && bsp.lumps[53].length) { variants.push_back(true); }
    scm::require(!variants.empty() && (options.hdr != "both" || variants.size() == 2), "The requested lighting variant is absent from the BSP.");
    cancellation.check(); progress(options, "starting_renderer");
    TextureStore store(names);
    scm::Json scenes = scm::Json::array(), rendererStatistics;
    double assetsSeconds = 0, prepareSeconds = 0, renderSeconds = 0, encodeWaitSeconds = 0;
    std::atomic<double> encodeSeconds = 0;
    size_t reusedCaptures = 0;
    scm::Stopwatch mounting;
    scm::Assets assets(bsp, options.game, options.mounts, options.profile);
    assetsSeconds += mounting.seconds();
    auto shaders = scm::digest(scm::readFile(shaderDirectory() / "scene.vert.spv")) + scm::digest(scm::readFile(shaderDirectory() / "scene.frag.spv"));
    scm::fs::path manifestFile;
    if (!options.noCache) { manifestFile = buildManifest(options, bsp, assets, shaders); }
    auto adapters = identifying.get();
    std::optional<ReusedBuild> reused;
    if (reuseAllowed) {
        scm::Stopwatch checking;
        reused = reuseBuild(options, bsp, assets, variants, manifestFile, adapters, store);
        assetsSeconds += checking.seconds();
        if (!reused) { starting = startRenderer(); }
    }
    if (reused) {
        spdlog::info("All {} captures are unchanged; reusing them without rendering", reused->captures);
        scenes = std::move(reused->scenes); rendererStatistics = std::move(reused->renderer); reusedCaptures = reused->captures;
    } else {
        auto load = [&](bool hdr) {
            cancellation.check();
            scm::Stopwatch loading;
            LoadedScene result;
            result.defaultSky = needsDefaultSky(bsp, assets, hdr, boundNamespaces, options.experimental);
            auto settings = sceneSettings(options, hdr, result.defaultSky);
            result.scene = scm::loadScene(bsp, assets, settings);
            result.sceneKey = scm::sceneCacheKey(bsp, assets, settings);
            result.assets = assets.report();
            result.seconds = loading.seconds();
            return result;
        };
        scm::Json manifestVariants = scm::Json::array();
        EncoderPool encoders;
        auto loading = std::async(std::launch::async, load, bool(variants.front()));
        for (size_t variant = 0; variant < variants.size(); ++variant) {
            bool hdr = variants[variant];
            cancellation.check(); progress(options, hdr ? "loading_hdr_assets" : "loading_ldr_assets");
            spdlog::info("Resolving {} scene and required assets", hdr ? "HDR" : "LDR");
            auto loaded = loading.get();
            cancellation.check();
            assetsSeconds += loaded.seconds;
            if (variant + 1 < variants.size()) { loading = std::async(std::launch::async, load, bool(variants[variant + 1])); }
            if (!renderer) { renderer = starting.get(); }
            auto statistics = renderer->statistics();
            auto cacheDescription = "renderer-v2:" + loaded.sceneKey + loaded.assets.dump() + options.quality + statistics.at("device").get<std::string>() + statistics.at("driver_version").dump() + shaders;
            auto cacheKey = scm::digest(std::as_bytes(std::span(cacheDescription.data(), cacheDescription.size())));
            auto requests = captureRequests(bsp, hdr, loaded.defaultSky);
            bool prepared = false;
            for (size_t i = 0; i < requests.size(); ++i) {
                const auto& request = requests[i];
                cancellation.check();
                progress(options, hdr ? "capturing_hdr" : "capturing_ldr", static_cast<unsigned>(i), static_cast<unsigned>(requests.size()));
                auto cacheFile = captureFile(options, cacheKey, request.name);
                if (options.incremental && !options.noCache) {
                    if (auto cached = readCapture(cacheFile, request.probe)) {
                        ++reusedCaptures;
                        if (!options.previews.empty()) { scm::writePreview(scm::decodeCube(*cached, hdr), options.previews, request.name); }
                        store.add(request.name, std::move(*cached));
                        continue;
                    }
                }
                if (!prepared) { progress(options, "preparing_gpu"); scm::Stopwatch preparation; renderer->prepare(loaded.scene); prepareSeconds += preparation.seconds(); prepared = true; cancellation.check(); }
                spdlog::info("Capturing {} {}/{}: {} ({} pixels)", hdr ? "HDR" : "LDR", i + 1, requests.size(), request.name, request.probe.resolution);
                scm::Stopwatch rendering;
                auto cube = renderer->capture(loaded.scene, request.probe, request.skyOnly);
                renderSeconds += rendering.seconds();
                auto cubeBytes = size_t(cube.resolution) * cube.resolution * 6 * sizeof(glm::vec4);
                encoders.submit(cubeBytes, [&options, &store, &encodeSeconds, hdr, name = request.name, cacheFile = std::move(cacheFile), cube = std::move(cube)]() mutable {
                    if (!options.previews.empty()) { scm::writePreview(cube, options.previews, name); }
                    scm::Stopwatch encoding;
                    auto bytes = scm::encodeCube(std::move(cube), hdr);
                    encodeSeconds += encoding.seconds();
                    if (!options.noCache) {
                        auto hash = scm::digest(bytes); scm::Bytes cached; cached.reserve(hash.size() + bytes.size());
                        auto prefix = std::as_bytes(std::span(hash.data(), hash.size()));
                        cached.insert(cached.end(), prefix.begin(), prefix.end()); cached.insert(cached.end(), bytes.begin(), bytes.end());
                        scm::queueCacheFile(cacheFile, std::move(cached));
                    }
                    store.add(name, std::move(bytes));
                });
            }
            scenes.push_back({{"hdr", hdr}, {"geometry", loaded.scene.diagnostics}, {"assets", loaded.assets}});
            manifestVariants.push_back({{"hdr", hdr}, {"default_sky", loaded.defaultSky}, {"scene_key", loaded.sceneKey}, {"cache_key", cacheKey}, {"geometry", loaded.scene.diagnostics}, {"assets", std::move(loaded.assets)}});
        }
        scm::Stopwatch waiting;
        encoders.finish();
        encodeWaitSeconds = waiting.seconds();
        rendererStatistics = renderer->statistics();
        if (!options.noCache && !adapters.empty()) {
            auto text = scm::Json{{"adapters", adapters}, {"renderer", rendererStatistics}, {"variants", std::move(manifestVariants)}}.dump();
            auto bytes = std::as_bytes(std::span(text.data(), text.size()));
            scm::queueCacheFile(manifestFile, scm::Bytes(bytes.begin(), bytes.end()));
        }
    }
    scm::Stopwatch writing;
    cancellation.check(); progress(options, "writing_bsp");
    auto textureCount = store.textures.size();
    auto metadata = scm::Json{{"schema", 1}, {"map_namespace", mapNamespace}, {"runtime_map_namespace", scm::lower(output.stem().string())}, {"texture_namespaces", names}}.dump(2);
    store.textures.emplace("sourcecubemap/build.json", std::as_bytes(std::span(metadata.data(), metadata.size())));
    auto validated = bsp.writeCubemaps(store.textures, output, options.overwrite, options.inPlace);
    times["bsp_writing_including_transaction_validation"] = writing.seconds();
    scm::Stopwatch validation;
    progress(options, "validating_output");
    scm::require(scm::digest(scm::readFile(output)) == validated, "The published BSP differs from the validated file.");
    times["validation"] = validation.seconds();
    times["startup"] = startupSeconds;
    times["scene_and_asset_loading"] = assetsSeconds; times["gpu_preparation"] = prepareSeconds;
    times["rendering"] = renderSeconds; times["encoding"] = encodeSeconds.load(); times["encoding_wait"] = encodeWaitSeconds; times["total"] = total.seconds();
    return {{"output", scm::fs::absolute(output).string()}, {"map_namespace", mapNamespace}, {"texture_namespaces", names}, {"probes", bsp.probes.size()}, {"textures_written", textureCount}, {"timings_seconds", times}, {"renderer", rendererStatistics}, {"scenes", scenes}, {"quality", options.quality}, {"capture_time", options.captureTime}, {"experimental", options.experimental}, {"rendering_fidelity", options.experimental ? "experimental_approximations" : "supported_subset_not_certified"}, {"structural_validation", "pass"}, {"native_game_acceptance", "not_performed"}, {"reused_captures", reusedCaptures}, {"cache_mode", options.incremental ? "incremental" : "full_regeneration"}};
}
}

int main(int argc, char** argv) {
    auto logger = spdlog::stderr_color_mt("sourcecubemap"); spdlog::set_default_logger(logger); spdlog::set_pattern("%v");
    CLI::App app{"Standalone Source Engine cubemap generator", "sourcecubemap"};
    app.set_version_flag("--version", "SourceCubemap " SCM_VERSION);
    app.require_subcommand(1);
    Options options;
    wchar_t* local = nullptr; size_t length = 0; _wdupenv_s(&local, &length, L"LOCALAPPDATA");
    options.cache = ((local ? scm::fs::path(local) : scm::fs::temp_directory_path()) / "SourceCubemap" / "cache").string(); std::free(local);
    auto common = [&](CLI::App* command, bool gameRequired) {
        command->add_option("map", options.input)->required()->check(CLI::ExistingFile);
        command->add_option("--game", options.game, "Installed game content directory.")->required(gameRequired)->check(CLI::ExistingDirectory);
        command->add_option("--profile", options.profile)->check(CLI::IsMember({"gmod", "hl2"}));
        command->add_option("--mount", options.mounts, "Additional content directories or archives, in priority order.");
        command->add_option("--json", options.report, "Save the complete machine-readable report.");
        command->add_flag("--verbose", options.verbose);
        command->add_flag("--progress-json", options.progressJson, "Emit newline-delimited progress events.");
    };
    auto inspect = app.add_subcommand("inspect", "Inspect BSP structure and cubemap samples."); common(inspect, false);
    inspect->add_option("--asset", options.asset, "Inspect a resolved text resource and its provenance.");
    auto validate = app.add_subcommand("validate", "Validate BSP bounds, embedded archives, and VTF images."); common(validate, false);
    auto generate = app.add_subcommand("build", "Render cubemaps and embed them into a new BSP."); common(generate, true);
    auto benchmark = app.add_subcommand("benchmark", "Measure complete regeneration, including encoding and BSP writing."); common(benchmark, true);
    for (auto* command : {generate, benchmark}) {
        command->add_option("--hdr", options.hdr)->check(CLI::IsMember({"auto", "ldr", "hdr", "both"}));
        command->add_option("--quality", options.quality)->check(CLI::IsMember({"fast", "standard", "high"}));
        command->add_option("--water", options.water, "Planar reflections and depth refraction require --experimental.")->check(CLI::IsMember({"strict", "planar"}));
        command->add_option("--capture-time", options.captureTime)->check(CLI::NonNegativeNumber);
        command->add_option("--cancel-event", options.cancelEvent, "Windows event used for cooperative cancellation before publication.");
        command->add_option("--device", options.device, "Case-insensitive Vulkan device name substring.");
        command->add_option("--cache", options.cache, "Persistent scene, pipeline, and capture cache directory.");
        command->add_option("--cache-limit-mb", options.cacheLimitMb, "Maximum cache size in MiB; writes also retain 8 GiB of free disk space.")->check(CLI::Range(0, 65536));
        command->add_flag("--no-cache", options.noCache, "Disable application cache reads and writes.");
        command->add_flag("--experimental", options.experimental, "Allow documented lighting and painted-sky approximations; output is not fidelity-validated.");
        command->add_option("--previews", options.previews, "Write six PNG previews per probe and lighting variant.");
    }
    generate->add_option("--output", options.output, "Destination BSP; defaults to MAP_built.bsp.");
    generate->add_flag("--overwrite", options.overwrite, "Replace an existing separate output.");
    generate->add_flag("--in-place", options.inPlace, "Replace the input after creating a backup.");
    generate->add_flag("--incremental", options.incremental, "Reuse captures whose map, dependencies, GPU, and settings are unchanged.");
    benchmark->add_option("--runs", options.runs)->check(CLI::Range(1, 100));
    try {
        app.parse(argc, argv);
        scm::setCacheBudget(uint64_t(options.cacheLimitMb) << 20);
        if (!options.report.empty()) { scm::require(!aliases(options.report, options.input), "The JSON report must not replace the input BSP."); }
        spdlog::set_level(options.verbose ? spdlog::level::debug : spdlog::level::info);
        scm::Json report;
        if (*generate) {
            auto output = options.output.empty() ? scm::fs::path(options.input).parent_path() / (scm::fs::path(options.input).stem().string() + "_built.bsp") : scm::fs::path(options.output);
            if (options.inPlace) { scm::require(options.output.empty(), "Use --in-place without --output."); output = options.input; }
            if (!options.report.empty()) { scm::require(!aliases(options.report, output), "The JSON report and output BSP must use different paths."); }
            report = build(options, output);
        } else if (*benchmark) {
            scm::Json runs = scm::Json::array(); std::vector<double> seconds;
            for (unsigned run = 0; run < options.runs; ++run) {
                scm::TemporaryFile output(".bsp"); options.overwrite = true;
                auto result = build(options, output.path); seconds.push_back(result["timings_seconds"]["total"].get<double>()); runs.push_back(std::move(result));
            }
            std::sort(seconds.begin(), seconds.end());
            auto median = seconds.size() % 2 ? seconds[seconds.size()/2] : (seconds[seconds.size()/2-1] + seconds[seconds.size()/2]) / 2;
            report = {{"mode", "full_regeneration"}, {"runs", runs}, {"median_seconds", median}, {"p95_seconds", seconds[size_t(std::ceil(0.95 * seconds.size())) - 1]}, {"unchanged_capture_reuse", false}};
        } else {
            scm::Bsp bsp(options.input);
            if (*validate) { bsp.validateTextures(); }
            report = bsp.inspect();
            if (!options.game.empty()) {
                scm::Assets assets(bsp, options.game, options.mounts, options.profile);
                if (!options.asset.empty()) { auto value = assets.get(options.asset, "inspect command"); report["requested_asset"] = {{"source", value->origin}, {"text", scm::textOf(value->bytes)}}; }
                auto strings = bsp.lump(43); scm::Reader names(strings, "material names"); report["materials"] = scm::Json::array();
                for (size_t offset = 0; offset < strings.size();) {
                    auto name = names.string(offset, strings.size() - offset); offset += name.size() + 1;
                    auto material = scm::readMaterial(assets, name, false);
                    report["materials"].push_back({{"name", name}, {"shader", material.shader}, {"parameters", material.parameters}, {"proxies", material.proxies}});
                }
                report["assets"] = assets.report();
            }
            report["validation"] = *validate ? "structural_pass" : "not_requested";
        }
        if (!options.report.empty()) { auto text = report.dump(2); scm::writeFile(options.report, std::as_bytes(std::span(text.data(), text.size()))); }
        if (options.progressJson) { std::cout << scm::Json{{"event", "complete"}, {"report", options.report}}.dump() << '\n'; }
        else { std::cout << report.dump(2) << '\n'; }
        return 0;
    } catch (const CLI::ParseError& error) { return app.exit(error); }
    catch (const std::exception& error) { spdlog::error("Error: {}", error.what()); return 1; }
}
