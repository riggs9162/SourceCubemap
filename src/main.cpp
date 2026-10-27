#include <scm/capture.hpp>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <Windows.h>
#include <iostream>
#include <set>
#include <cmath>

namespace {
struct Options {
    std::string input, game, output, report, asset, profile = "gmod", hdr = "auto", quality = "standard", device, previews, water = "strict";
    std::vector<std::string> mounts;
    bool overwrite = false, inPlace = false, verbose = false, experimental = false;
    double captureTime = 0;
    unsigned runs = 3;
};
scm::fs::path shaderDirectory() {
    std::wstring path(32768, L'\0');
    auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    scm::require(count > 0 && count < path.size(), "Cannot locate executable shaders.");
    path.resize(count);
    return scm::fs::path(path).parent_path() / "shaders";
}
std::set<std::string> namespaces(const scm::Bsp& bsp) {
    std::set<std::string> names;
    for (const auto& [name, original] : bsp.pakNames) {
        if (!name.starts_with("materials/maps/")) { continue; }
        auto end = name.find('/', 15);
        if (end != std::string::npos) { names.insert(name.substr(15, end - 15)); }
    }
    if (names.empty()) { names.insert(scm::lower(bsp.path.stem().string())); }
    scm::require(names.size() == 1, "Multiple embedded map namespaces require explicit binding resolution, which is not implemented yet.");
    return names;
}
scm::Json build(const Options& options, const scm::fs::path& output) {
    scm::Stopwatch total, parsing;
    scm::Bsp bsp(options.input);
    scm::require(!bsp.probes.empty(), "The map contains no cubemap samples.");
    auto names = namespaces(bsp);
    scm::Json times{{"parsing", parsing.seconds()}};
    std::vector<bool> variants;
    if ((options.hdr == "auto" || options.hdr == "ldr" || options.hdr == "both") && bsp.lumps[8].length) { variants.push_back(false); }
    if ((options.hdr == "auto" || options.hdr == "hdr" || options.hdr == "both") && bsp.lumps[53].length) { variants.push_back(true); }
    scm::require(!variants.empty() && (options.hdr != "both" || variants.size() == 2), "The requested lighting variant is absent from the BSP.");
    scm::Stopwatch startup;
    scm::Renderer renderer({options.device, options.quality, shaderDirectory()});
    times["startup"] = startup.seconds();
    std::map<std::string, scm::Bytes> textures;
    scm::Json scenes = scm::Json::array();
    double assetsSeconds = 0, prepareSeconds = 0, renderSeconds = 0, encodeSeconds = 0;
    for (bool hdr : variants) {
        spdlog::info("Resolving {} scene and required assets", hdr ? "HDR" : "LDR");
        scm::Stopwatch loading;
        scm::Assets assets(bsp, options.game, options.mounts, options.profile);
        auto scene = scm::loadScene(bsp, assets, {hdr, options.captureTime, {}, false, options.experimental});
        assetsSeconds += loading.seconds();
        scm::Stopwatch preparation;
        renderer.prepare(scene);
        prepareSeconds += preparation.seconds();
        for (size_t i = 0; i < bsp.probes.size(); ++i) {
            const auto& probe = bsp.probes[i];
            spdlog::info("Capturing {} probe {}/{}: {} ({} pixels)", hdr ? "HDR" : "LDR", i + 1, bsp.probes.size(), probe.name(), probe.resolution);
            scm::Stopwatch rendering;
            auto cube = renderer.capture(scene, probe);
            renderSeconds += rendering.seconds();
            scm::Stopwatch encoding;
            auto bytes = scm::encodeCube(cube, hdr);
            auto name = probe.name() + (hdr ? ".hdr" : "");
            for (const auto& map : names) { textures.emplace("materials/maps/" + map + "/" + name + ".vtf", bytes); }
            encodeSeconds += encoding.seconds();
            if (!options.previews.empty()) { scm::writePreview(cube, options.previews, name); }
        }
        scenes.push_back({{"hdr", hdr}, {"geometry", scene.diagnostics}, {"assets", assets.report()}});
    }
    scm::Stopwatch writing;
    bsp.writeCubemaps(textures, output, options.overwrite, options.inPlace);
    times["bsp_writing_including_transaction_validation"] = writing.seconds();
    scm::Stopwatch validation;
    scm::Bsp reopened(output); reopened.validateTextures(); reopened.verifyPreserved(bsp, textures);
    times["validation"] = validation.seconds();
    times["scene_and_asset_loading"] = assetsSeconds; times["gpu_preparation"] = prepareSeconds;
    times["rendering"] = renderSeconds; times["encoding"] = encodeSeconds; times["total"] = total.seconds();
    return {{"output", scm::fs::absolute(output).string()}, {"map_namespace", *names.begin()}, {"probes", bsp.probes.size()}, {"textures_written", textures.size()}, {"timings_seconds", times}, {"renderer", renderer.statistics()}, {"scenes", scenes}, {"quality", options.quality}, {"capture_time", options.captureTime}, {"structural_validation", "pass"}, {"native_game_acceptance", "not_performed"}, {"cache_mode", "full_regeneration"}};
}
}

int main(int argc, char** argv) {
    auto logger = spdlog::stderr_color_mt("sourcecubemap"); spdlog::set_default_logger(logger); spdlog::set_pattern("%v");
    CLI::App app{"Standalone Source Engine cubemap generator", "sourcecubemap"};
    app.set_version_flag("--version", "SourceCubemap 0.1.0");
    app.require_subcommand(1);
    Options options;
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
        command->add_option("--water", options.water, "Strict mode rejects unsupported water.")->check(CLI::IsMember({"strict"}));
        command->add_option("--capture-time", options.captureTime)->check(CLI::NonNegativeNumber);
        command->add_option("--device", options.device, "Case-insensitive Vulkan device name substring.");
        command->add_flag("--experimental", options.experimental, "Allow documented lighting and painted-sky approximations; output is not fidelity-validated.");
        command->add_option("--previews", options.previews, "Write six PNG previews per probe and lighting variant.");
    }
    generate->add_option("--output", options.output, "Destination BSP; defaults to MAP_built.bsp.");
    generate->add_flag("--overwrite", options.overwrite, "Replace an existing separate output.");
    generate->add_flag("--in-place", options.inPlace, "Replace the input after creating a backup.");
    benchmark->add_option("--runs", options.runs)->check(CLI::Range(1, 100));
    try {
        app.parse(argc, argv);
        spdlog::set_level(options.verbose ? spdlog::level::debug : spdlog::level::info);
        scm::Json report;
        if (*generate) {
            auto output = options.output.empty() ? scm::fs::path(options.input).parent_path() / (scm::fs::path(options.input).stem().string() + "_built.bsp") : scm::fs::path(options.output);
            if (options.inPlace) { scm::require(options.output.empty(), "Use --in-place without --output."); output = options.input; }
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
