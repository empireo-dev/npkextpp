// npkextpp: command line front end.
//
//   locate installation → read resdb → plan → (one entry | pipeline) → report
#include "cli_options.hpp"
#include "licenses.hpp"
#include "progress_view.hpp"

#include "npk/install.hpp"
#include "npk/names.hpp"
#include "npk/plan.hpp"
#include "npk/pipeline/pipeline.hpp"

#include <cstdio>
#include <iostream>

int main(int argc, char** argv)
{
    int exit_code = 0;
    const auto options = npkextpp::parse_cli(argc, argv, exit_code);
    if (!options) return exit_code;

    if (options->show_licenses) {
        std::cout << npkextpp::third_party_licenses();
        return 0;
    }

    // TODO: Installation::locate → NameResolver from resdb → Planner::build →
    //       single entry via npk::read_entry, or npk::pipeline::run for the plan.
    std::cerr << "npkextpp " NPKEXTPP_VERSION ": not implemented yet\n";
    return 1;
}
