#include <scm/capture.hpp>
#include <scm/cache.hpp>
#include <vtfpp/VTF.h>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <Windows.h>
#include <iostream>
#include <set>
#include <cmath>

namespace {
struct Options {
    std::string input, game, output, report, asset, cache, profile = "gmod", hdr = "auto", quality = "standard", device, previews, water = "strict";
    std::vector<std::string> mounts;
    bool overwrite = false, inPlace = false, verbose = false, experimental = false, noCache = false, incremental = false;
    double captureTime = 0;
    unsigned runs = 3;
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
    scm::require(names.size() == 1, "Multiple embedded map namespaces require explicit binding resolution, which is not implemented yet.");
    return names;
}
bool needsDefaultSky(const scm::Bsp& bsp, scm::Assets& assets, bool hdr, std::string_view mapName) {
    auto prefix = "maps/" + std::string(mapName) + "/";
    std::set<std::string> probes;
    for (const auto& probe : bsp.probes) { probes.insert(prefix + probe.name()); }
    bool needed = bsp.pakNames.contains("materials/" + prefix + "cubemapdefault.vtf") || bsp.pakNames.contains("materials/" + prefix + "cubemapdefault.hdr.vtf");
    auto names = bsp.lump(43); scm::Reader strings(names, "compiled material names");
    for (size_t offset = 0; offset < names.size();) {
        auto name = strings.string(offset, names.size() - offset); offset += name.size() + 1;
        auto material = scm::readMaterial(assets, name, hdr);
        auto binding = material.value("$envmap");
        if (binding.empty()) { continue; }
        binding = scm::assetPath(binding);
        if (binding.starts_with("materials/")) { binding.erase(0, 10); }
        if (binding.ends_with(".vtf")) { binding.resize(binding.size() - 4); }
        if (binding.ends_with(".hdr")) { binding.resize(binding.size() - 4); }
        if (!binding.starts_with("maps/")) { continue; }
        if (binding == prefix + "cubemapdefault") { needed = true; }
        else { scm::require(probes.contains(binding), std::format("Material '{}' references cubemap '{}' without a matching BSP sample. The binding was preserved; output was not written.", name, binding)); }
    }
    return needed;
}
scm::Json build(const Options& options, const scm::fs::path& output) {
    scm::require(std::isfinite(options.captureTime) && options.captureTime >= 0, "Capture time must be a finite nonnegative number.");
    scm::Stopwatch total, parsing;
    scm::Bsp bsp(options.input);
    bsp.validateDestination(output, options.overwrite, options.inPlace);
    scm::require(!bsp.probes.empty(), "The map contains no cubemap samples.");
    auto names = namespaces(bsp);
    auto mapNamespace = *names.begin();
    names.insert(scm::lower(output.stem().string()));
    scm::Json times{{"parsing", parsing.seconds()}};
    std::vector<bool> variants;
    if ((options.hdr == "auto" || options.hdr == "ldr" || options.hdr == "both") && bsp.lumps[8].length) { variants.push_back(false); }
    if ((options.hdr == "auto" || options.hdr == "hdr" || options.hdr == "both") && bsp.lumps[53].length) { variants.push_back(true); }
    scm::require(!variants.empty() && (options.hdr != "both" || variants.size() == 2), "The requested lighting variant is absent from the BSP.");
    scm::Stopwatch startup;
    scm::Renderer renderer({options.device, options.quality, shaderDirectory(), options.noCache ? scm::fs::path{} : scm::fs::path(options.cache)});
    times["startup"] = startup.seconds();
    std::map<std::string, scm::Bytes> textures;
    scm::Json scenes = scm::Json::array();
    double assetsSeconds = 0, prepareSeconds = 0, renderSeconds = 0, encodeSeconds = 0;
    size_t reusedCaptures = 0;
    scm::Stopwatch mounting;
    scm::Assets assets(bsp, options.game, options.mounts, options.profile);
    assetsSeconds += mounting.seconds();
    for (bool hdr : variants) {
        spdlog::info("Resolving {} scene and required assets", hdr ? "HDR" : "LDR");
        scm::Stopwatch loading;
        bool defaultSky = needsDefaultSky(bsp, assets, hdr, mapNamespace);
        scm::SceneOptions sceneOptions{hdr, options.captureTime, options.cache, !options.noCache, options.experimental, defaultSky};
        sceneOptions.planarWater = options.water == "planar";
        auto scene = scm::loadScene(bsp, assets, sceneOptions);
        assetsSeconds += loading.seconds();
        auto cacheDescription = "renderer-v2:" + scm::sceneCacheKey(bsp, assets, sceneOptions) + assets.report().dump() + options.quality + renderer.statistics().at("device").get<std::string>() + renderer.statistics().at("driver_version").dump() + scm::digest(scm::readFile(shaderDirectory() / "scene.vert.spv")) + scm::digest(scm::readFile(shaderDirectory() / "scene.frag.spv"));
        auto cacheKey = scm::digest(std::as_bytes(std::span(cacheDescription.data(), cacheDescription.size())));
        bool prepared = false;
        for (size_t i = 0; i < bsp.probes.size() + size_t(defaultSky); ++i) {
            bool skyOnly = i == bsp.probes.size();
            scm::Probe probe = skyOnly ? scm::Probe{glm::ivec3(0), 32} : bsp.probes[i];
            auto name = (skyOnly ? std::string("cubemapdefault") : probe.name()) + (hdr ? ".hdr" : "");
            auto cacheFile = scm::fs::path(options.cache) / "captures" / cacheKey / (name + ".bin");
            scm::Bytes bytes;
            if (options.incremental && !options.noCache && scm::fs::exists(cacheFile)) {
                try {
                    auto cached = scm::readFile(cacheFile);
                    scm::require(cached.size() >= 16, "Truncated capture cache.");
                    auto payload = std::span<const std::byte>(cached).subspan(16);
                    scm::require(scm::digest(payload) == scm::textOf(std::span<const std::byte>(cached).first(16)), "Capture cache checksum mismatch.");
                    scm::validateVtf(payload, cacheFile.string());
                    bytes.assign(payload.begin(), payload.end()); vtfpp::VTF vtf(bytes);
                    scm::require(static_cast<bool>(vtf) && vtf.getFaceCount() == 7 && vtf.getWidth() == probe.resolution && vtf.getHeight() == probe.resolution, "Invalid cached cubemap.");
                    ++reusedCaptures;
                } catch (const std::exception& error) { spdlog::warn("Ignoring capture cache: {}", error.what()); bytes.clear(); }
            }
            if (!bytes.empty()) {
                for (const auto& map : names) { textures.emplace("materials/maps/" + map + "/" + name + ".vtf", bytes); }
                if (!options.previews.empty()) { scm::writePreview(scm::decodeCube(bytes, hdr), options.previews, name); }
                continue;
            }
            if (!prepared) { scm::Stopwatch preparation; renderer.prepare(scene); prepareSeconds += preparation.seconds(); prepared = true; }
            spdlog::info("Capturing {} {}/{}: {} ({} pixels)", hdr ? "HDR" : "LDR", i + 1, bsp.probes.size() + size_t(defaultSky), name, probe.resolution);
            scm::Stopwatch rendering;
            auto cube = renderer.capture(scene, probe, skyOnly);
            renderSeconds += rendering.seconds();
            scm::Stopwatch encoding;
            bytes = scm::encodeCube(cube, hdr);
            for (const auto& map : names) { textures.emplace("materials/maps/" + map + "/" + name + ".vtf", bytes); }
            encodeSeconds += encoding.seconds();
            if (!options.noCache) {
                try { auto hash = scm::digest(bytes); scm::Bytes cached; auto prefix = std::as_bytes(std::span(hash.data(), hash.size())); cached.insert(cached.end(), prefix.begin(), prefix.end()); cached.insert(cached.end(), bytes.begin(), bytes.end()); scm::writeCacheFile(cacheFile, cached); }
                catch (const std::exception& error) { spdlog::warn("Capture cache write failed: {}", error.what()); }
            }
            if (!options.previews.empty()) { scm::writePreview(cube, options.previews, name); }
        }
        scenes.push_back({{"hdr", hdr}, {"geometry", scene.diagnostics}, {"assets", assets.report()}});
    }
    scm::Stopwatch writing;
    auto textureCount = textures.size();
    auto metadata = scm::Json{{"schema", 1}, {"map_namespace", mapNamespace}, {"runtime_map_namespace", scm::lower(output.stem().string())}, {"texture_namespaces", names}}.dump(2);
    auto metadataBytes = std::as_bytes(std::span(metadata.data(), metadata.size()));
    textures.emplace("sourcecubemap/build.json", scm::Bytes(metadataBytes.begin(), metadataBytes.end()));
    bsp.writeCubemaps(textures, output, options.overwrite, options.inPlace);
    times["bsp_writing_including_transaction_validation"] = writing.seconds();
    scm::Stopwatch validation;
    scm::Bsp reopened(output); reopened.validateTextures(); reopened.verifyPreserved(bsp, textures);
    times["validation"] = validation.seconds();
    times["scene_and_asset_loading"] = assetsSeconds; times["gpu_preparation"] = prepareSeconds;
    times["rendering"] = renderSeconds; times["encoding"] = encodeSeconds; times["total"] = total.seconds();
    return {{"output", scm::fs::absolute(output).string()}, {"map_namespace", mapNamespace}, {"texture_namespaces", names}, {"probes", bsp.probes.size()}, {"textures_written", textureCount}, {"timings_seconds", times}, {"renderer", renderer.statistics()}, {"scenes", scenes}, {"quality", options.quality}, {"capture_time", options.captureTime}, {"experimental", options.experimental}, {"rendering_fidelity", options.experimental ? "experimental_approximations" : "supported_subset_not_certified"}, {"structural_validation", "pass"}, {"native_game_acceptance", "not_performed"}, {"reused_captures", reusedCaptures}, {"cache_mode", options.incremental ? "incremental" : "full_regeneration"}};
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
        command->add_option("--device", options.device, "Case-insensitive Vulkan device name substring.");
        command->add_option("--cache", options.cache, "Persistent scene, pipeline, and capture cache directory.");
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
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const CLI::ParseError& error) { return app.exit(error); }
    catch (const std::exception& error) { spdlog::error("Error: {}", error.what()); return 1; }
}
