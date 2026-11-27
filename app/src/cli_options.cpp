#include "cli_options.hpp"

#include <CLI11.hpp>

namespace npkextpp {

namespace {

constexpr const char* kDescription =
    "Unpack NeoX NXPK (.npk) archives. The output mirrors the game's file tree: res.npk goes to "
    "<out>/res/, res/ui.npk to <out>/res/ui/, res/model/character.npk to <out>/res/model/character/. "
    "Archives mounted at the same place from several folders (res, res2, Documents/res, ...) are merged, "
    "the higher-priority one winning. <out>/_archives/ records every archive, so they can be rebuilt; "
    "unpacking more archives later into the same <out> adds to it. Files already in <out> are kept, "
    "so an interrupted run continues where it stopped (--overwrite writes everything again).";

constexpr const char* kExamples = R"(
examples:
  # everything in the installation
  npkextpp --install "path/to/Magic Awakened" --out unpacked

  # some archives, by name: found anywhere in the installation, all mount folders merged
  npkextpp --install "path/to/Magic Awakened" --in character.npk ui "res_dlc_*" --out unpacked

  # one specific mount folder: a path inside the installation
  npkextpp --install "path/to/Magic Awakened" --in Documents/res/ui.npk --out unpacked

  # one file, by its path in the game's tree
  npkextpp --install "path/to/Magic Awakened" --entry ui/login/bg.png --out .

  # show what would be unpacked, write nothing
  npkextpp --install "path/to/Magic Awakened" --dry-run

  # archives given by their file path
  npkextpp --in path/to/res/model/texture.npk --out unpacked
  npkextpp --in copy/ui.npk --resdb path/to/res/resdb --out unpacked

  # replace one file inside an archive with a new version
  npkextpp update --new my_bg.png --path login/bg.png --target "path/to/Magic Awakened/res/ui.npk"

  # replace every file of a folder laid out like the archive (changed ones only)
  npkextpp update --in unpacked/res/ui --target "path/to/Magic Awakened/res/ui.npk"
)";

} // namespace

std::optional<CliOptions> parse_cli(int argc, char** argv, int& exit_code)
{
    CliOptions options;
    CLI::App app{kDescription, "npkextpp"};
    app.footer(kExamples);
    app.set_version_flag("--version", "npkextpp " NPKEXTPP_VERSION);
    app.get_formatter()->column_width(28);

    auto* source = "where to read";
    app.add_option("--install", options.install, "game installation folder (contains neox_bin/, res/, Documents/)")
        ->group(source);
    app.add_option("--in", options.inputs,
                   "archives: file paths, names searched in the installation (ui, 'res_dlc_*'), "
                   "or paths inside it (Documents/res/ui.npk). Default: every archive")
        ->group(source)
        ->expected(1, -1);
    app.add_option("--resdb", options.resdb, "name manifests to use instead of the installation's")
        ->group(source)
        ->expected(1, -1);

    auto* selection = "what to unpack";
    app.add_option("--unpack", options.unpack, "only archives with these names (wildcards allowed). Default: all")
        ->group(selection)
        ->expected(1, -1);
    app.add_option("--limit", options.limit, "only the first N entries of each archive (for quick tests)")
        ->group(selection)
        ->check(CLI::PositiveNumber);
    app.add_option("--entry", options.entry, "extract only this file, by its path in the game's tree")
        ->group(selection);

    auto* output = "where to write";
    app.add_option("--out", options.out, "output folder; archives are unpacked into folders below it")->group(output);
    app.add_flag("--overwrite", options.overwrite,
                 "write every file again, also those already in the output folder (default: keep them)")
        ->group(output);
    app.add_flag("--keep-overridden", options.keep_overridden,
                 "also write lower-priority copies of files, to <folder>/_overridden/<archive>/")
        ->group(output);

    auto* other = "other";
    app.add_flag("--dry-run", options.dry_run, "show the plan and stop; nothing is written")->group(other);
    bool no_verify = false;
    app.add_flag("--no-verify", no_verify, "skip the checksum checks (slightly faster, not recommended)")->group(other);
    app.add_flag("-v,--verbose", options.verbose, "print the tuner's decisions and other details")->group(other);
    app.add_flag("--licenses", options.show_licenses, "show the licenses of the bundled libraries")->group(other);

    UpdateOptions update;
    CLI::App* update_command = app.add_subcommand(
        "update", "replace files inside an archive with new versions (the archive is changed in place: keep a copy)");
    auto* new_option = update_command->add_option("--new", update.new_file, "the new version of one file")
                           ->check(CLI::ExistingFile);
    auto* path_option = update_command->add_option(
        "--path", update.path, "its path in the archive (login/bg.png in ui.npk; res/ui/login/bg.png works too)");
    auto* in_option = update_command->add_option(
        "--in", update.folder,
        "a folder laid out like the archive: every file in it replaces the one at the same path "
        "(unpacked/res/ui for ui.npk); files identical to the archive's are left alone")
                          ->check(CLI::ExistingDirectory);
    new_option->needs(path_option)->excludes(in_option);
    path_option->needs(new_option);
    update_command->add_option("--target", update.target, "the archive to change")->required()->check(CLI::ExistingFile);
    update_command->callback([&] {
        if (!update.new_file && !update.folder) throw CLI::RequiredError("--new FILE --path PATH, or --in FOLDER,");
    });
    app.require_subcommand(0, 1);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        exit_code = app.exit(error);
        if (exit_code != 0) exit_code = kExitBadInput;
        return std::nullopt;
    }
    options.verify = !no_verify;
    if (update_command->parsed()) options.update = update;
    exit_code = kExitOk;
    return options;
}

} // namespace npkextpp
