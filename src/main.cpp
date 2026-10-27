#include <scm/bsp.hpp>
#include <scm/assets.hpp>
#include <CLI/CLI.hpp>
#include <iostream>

int main(int argc, char** argv) {
    CLI::App app{"Standalone Source Engine cubemap generator", "sourcecubemap"};
    app.set_version_flag("--version", "SourceCubemap 0.1.0");
    app.require_subcommand(1);
    std::string input;
    std::string game, profile = "gmod";
    std::vector<std::string> mounts;
    auto inspect = app.add_subcommand("inspect", "Inspect BSP structure and cubemap samples.");
    inspect->add_option("map", input)->required()->check(CLI::ExistingFile);
    inspect->add_option("--game", game);
    inspect->add_option("--profile", profile)->check(CLI::IsMember({"gmod", "hl2"}));
    inspect->add_option("--mount", mounts);
    auto validate = app.add_subcommand("validate", "Validate BSP bounds, embedded assets, and VTF images.");
    validate->add_option("map", input)->required()->check(CLI::ExistingFile);
    try {
        app.parse(argc, argv);
        scm::Bsp bsp(input);
        if (*validate) { bsp.validateTextures(); }
        auto report = bsp.inspect();
        if (!game.empty()) {
            scm::Assets assets(bsp, game, mounts, profile);
            auto strings = bsp.lump(43);
            scm::Reader names(strings, "material names");
            report["materials"] = scm::Json::array();
            for (size_t offset = 0; offset < strings.size();) {
                auto name = names.string(offset, strings.size() - offset);
                offset += name.size() + 1;
                auto material = scm::readMaterial(assets, name, false);
                report["materials"].push_back({{"name", name}, {"shader", material.shader}, {"parameters", material.parameters}, {"proxies", material.proxies}});
            }
            report["assets"] = assets.report();
        }
        report["validation"] = *validate ? "structural_pass" : "not_requested";
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const CLI::ParseError& error) { return app.exit(error); }
    catch (const std::exception& error) { std::cerr << "Error: " << error.what() << '\n'; return 1; }
}
