#include "binder/query/reading_clause/bound_unwind_clause.h"
#include "planner/operator/logical_unwind.h"
#include "planner/planner.h"

using namespace kuzu::binder;

namespace kuzu {
namespace planner {

void Planner::appendUnwind(const BoundReadingClause& boundReadingClause, LogicalPlan& plan) {
    auto& boundUnwindClause = (BoundUnwindClause&)boundReadingClause;
    auto unwind = make_shared<LogicalUnwind>(
        boundUnwindClause.getInExpr(), boundUnwindClause.getOutExpr(), plan.getLastOperator());
    appendFlattens(unwind->getGroupsPosToFlatten(), plan);
    unwind->setChild(0, plan.getLastOperator());
    unwind->computeFactorizedSchema();
    plan.setLastOperator(unwind);
}

} // namespace planner
} // namespace kuzu
