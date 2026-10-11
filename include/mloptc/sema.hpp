#pragma once

#include "mloptc/ast.hpp"
#include "mloptc/diagnostics.hpp"

namespace mloptc {

/// Semantic analysis: everything that is wrong with a program but still parses.
///
/// It does four jobs in one walk over the AST:
///
///  1. **Name resolution.** Every VarRefExpr / AssignStmt / subscript is
///     resolved to a (scope, slot) pair, and every CallExpr to a function
///     index. After this pass no later stage ever looks a name up again, which
///     is what keeps the interpreter and the IR lowering simple and fast.
///
///  2. **Slot assignment.** Each function gets a flat frame: parameters first,
///     then one slot per local declaration *anywhere* in the body. Shadowed and
///     out-of-scope locals get distinct slots, so lexical scoping is resolved
///     entirely at compile time and nothing about scopes survives into
///     execution.
///
///  3. **Type checking.** ToyC has few rules -- an array is not a value, a void
///     call is not a value, arities must match -- but each must be enforced,
///     because the backends assume a well-typed AST and would otherwise
///     misbehave on input the front end should have rejected.
///
///  4. **Control-flow validity.** `break`/`continue` only inside a loop,
///     `return expr` only in an int function, and a `main` that exists with the
///     right signature.
///
/// Returns true if the program is valid. On false, `diags` holds the reasons.
[[nodiscard]] bool analyze(Program& program, DiagnosticEngine& diags);

} // namespace mloptc
