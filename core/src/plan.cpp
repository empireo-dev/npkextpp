#include "npk/plan.hpp"

namespace npk {

Planner::Planner(const Installation& installation, NameResolver& names)
    : installation_(installation)
    , names_(names)
{
}

Result<Plan> Planner::build(const PlanOptions& options)
{
    (void)options;
    (void)installation_;
    (void)names_;
    return make_error(Errc::not_implemented, "Planner::build");
}

} // namespace npk
