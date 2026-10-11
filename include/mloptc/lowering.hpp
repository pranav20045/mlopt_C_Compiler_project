#pragma once

#include "mloptc/ast.hpp"
#include "mloptc/ir.hpp"

namespace mloptc {

/// Translates a type-checked AST into IR.
///
/// This is where structured control flow becomes a control-flow graph: `if`,
/// `while`, `for`, `break`, `continue` and short-circuit `&&`/`||` all turn
/// into basic blocks connected by jumps and branches. After this point the
/// compiler no longer knows what a loop is -- only that some blocks branch
/// backwards -- which is exactly the level at which the optimization passes
/// want to work.
///
/// The program must have passed `analyze()` first.
[[nodiscard]] ir::Module lower(const Program& program);

} // namespace mloptc
