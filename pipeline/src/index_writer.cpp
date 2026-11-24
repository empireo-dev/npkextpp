#include "npk/pipeline/index_writer.hpp"

namespace npk::pipeline {

Result<void> write_index(const Plan& plan, const std::filesystem::path& output_root)
{
    (void)plan;
    (void)output_root;
    return make_error(Errc::not_implemented, "write_index");
}

} // namespace npk::pipeline
