#include "cli_options.hpp"

#include <CLI11.hpp>

namespace npkextpp {

std::optional<CliOptions> parse_cli(int argc, char** argv, int& exit_code)
{
    CliOptions options;
    CLI::App app{"Extractor for NeoX NXPK (.npk) archives", "npkextpp"};
    app.set_version_flag("--version", NPKEXTPP_VERSION);

    app.add_option("--install", options.install, "Game installation folder");
    app.add_option("--out", options.out, "Output folder");
    app.add_option("--in", options.only, "Only these archives (names or parts of names)");
    app.add_option("--entry", options.entry, "Extract a single file by its path");
    app.add_flag("--keep-overridden", options.keep_overridden,
                 "Also write lower-priority copies of files");
    app.add_flag("!--no-verify", options.verify, "Skip checksum verification");
    app.add_flag("-v,--verbose", options.verbose, "Print tuner decisions and details");
    app.add_flag("--licenses", options.show_licenses, "Show third-party licenses");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        exit_code = app.exit(error);
        return std::nullopt;
    }
    exit_code = 0;
    return options;
}

} // namespace npkextpp
