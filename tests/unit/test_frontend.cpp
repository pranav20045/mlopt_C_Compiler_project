#include "mloptc/driver.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace mloptc;

namespace {

/// Returns the concatenated error messages, or "" if the program is valid.
std::string errorsFor(std::string_view source) {
  DiagnosticEngine diags;
  auto program = compileFrontend(source, diags);
  if (program) {
    return "";
  }
  std::string all;
  for (const Diagnostic& d : diags.all()) {
    if (d.severity == Severity::Error) {
      all += d.message + "\n";
    }
  }
  return all;
}

::testing::AssertionResult rejects(std::string_view source, std::string_view expectedSubstring) {
  const std::string errors = errorsFor(source);
  if (errors.empty()) {
    return ::testing::AssertionFailure() << "expected a rejection, but the program compiled";
  }
  if (errors.find(expectedSubstring) == std::string::npos) {
    return ::testing::AssertionFailure()
           << "expected an error containing \"" << expectedSubstring << "\", got:\n"
           << errors;
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult accepts(std::string_view source) {
  const std::string errors = errorsFor(source);
  if (!errors.empty()) {
    return ::testing::AssertionFailure() << "expected acceptance, got:\n" << errors;
  }
  return ::testing::AssertionSuccess();
}

} // namespace

// =============================================================================
// Programs that must be accepted
// =============================================================================

TEST(Frontend, AcceptsMinimalProgram) {
  EXPECT_TRUE(accepts("int main() { return 0; }"));
}

TEST(Frontend, AcceptsShadowingInInnerScope) {
  EXPECT_TRUE(accepts("int main() { int x = 1; { int x = 2; print(x); } return 0; }"));
}

TEST(Frontend, AcceptsArrayParameter) {
  EXPECT_TRUE(accepts("void f(int a[]) { a[0] = 1; } "
                      "int main() { int b[2]; f(b); return 0; }"));
}

TEST(Frontend, AcceptsMutualRecursion) {
  EXPECT_TRUE(accepts("int a(int n) { return b(n); } "
                      "int b(int n) { return n; } "
                      "int main() { return a(1); }"));
}

TEST(Frontend, AcceptsEmptyStatement) {
  EXPECT_TRUE(accepts("int main() { ; return 0; }"));
}

// =============================================================================
// Program structure
// =============================================================================

TEST(Frontend, RejectsMissingMain) {
  EXPECT_TRUE(rejects("int f() { return 0; }", "no 'main'"));
}

TEST(Frontend, RejectsMainWithParameters) {
  EXPECT_TRUE(rejects("int main(int a) { return 0; }", "'main' must take no parameters"));
}

TEST(Frontend, RejectsVoidMain) {
  EXPECT_TRUE(rejects("void main() { }", "'main' must return int"));
}

TEST(Frontend, RejectsDuplicateFunction) {
  EXPECT_TRUE(rejects("int f() { return 0; } int f() { return 1; } int main() { return 0; }",
                      "redefinition of function"));
}

TEST(Frontend, RejectsDuplicateGlobal) {
  EXPECT_TRUE(rejects("int g; int g; int main() { return 0; }", "redefinition of global"));
}

TEST(Frontend, RejectsVoidVariable) {
  EXPECT_TRUE(rejects("void g; int main() { return 0; }", "cannot have type 'void'"));
}

// =============================================================================
// Names and scope
// =============================================================================

TEST(Frontend, RejectsUndeclaredVariable) {
  EXPECT_TRUE(rejects("int main() { print(x); return 0; }", "undeclared identifier 'x'"));
}

TEST(Frontend, RejectsAssignmentToUndeclared) {
  EXPECT_TRUE(rejects("int main() { x = 1; return 0; }", "undeclared variable 'x'"));
}

TEST(Frontend, RejectsRedeclarationInSameScope) {
  EXPECT_TRUE(rejects("int main() { int x; int x; return 0; }", "redeclaration of 'x'"));
}

TEST(Frontend, RejectsSelfReferentialInitialiser) {
  // `int x = x;` must not silently read an uninitialised slot.
  EXPECT_TRUE(rejects("int main() { int x = x; return 0; }", "undeclared identifier 'x'"));
}

TEST(Frontend, RejectsUseOfVariableAfterItsScopeEnds) {
  EXPECT_TRUE(
      rejects("int main() { { int x = 1; } print(x); return 0; }", "undeclared identifier 'x'"));
}

TEST(Frontend, RejectsFunctionUsedAsValue) {
  EXPECT_TRUE(rejects("int f() { return 1; } int main() { print(f); return 0; }",
                      "did you mean to call it"));
}

// =============================================================================
// Calls
// =============================================================================

TEST(Frontend, RejectsUnknownFunction) {
  EXPECT_TRUE(rejects("int main() { return nope(); }", "undeclared function 'nope'"));
}

TEST(Frontend, RejectsWrongArgumentCount) {
  EXPECT_TRUE(rejects("int f(int a, int b) { return 0; } int main() { return f(1); }",
                      "expects 2 argument(s)"));
}

TEST(Frontend, RejectsVoidCallAsValue) {
  EXPECT_TRUE(rejects("void f() { } int main() { print(f()); return 0; }",
                      "used where a value is required"));
}

TEST(Frontend, AcceptsVoidCallAsStatement) {
  EXPECT_TRUE(accepts("void f() { } int main() { f(); return 0; }"));
}

// =============================================================================
// Arrays
// =============================================================================

TEST(Frontend, RejectsArrayAsValue) {
  EXPECT_TRUE(rejects("int main() { int a[3]; print(a); return 0; }", "is not a value"));
}

TEST(Frontend, RejectsWholeArrayAssignment) {
  EXPECT_TRUE(
      rejects("int main() { int a[3]; int b[3]; a = b; return 0; }", "cannot assign to array"));
}

TEST(Frontend, RejectsSubscriptOfNonArray) {
  EXPECT_TRUE(rejects("int main() { int x; print(x[0]); return 0; }", "is not an array"));
}

TEST(Frontend, RejectsIntArgumentForArrayParameter) {
  EXPECT_TRUE(rejects("void f(int a[]) { } int main() { f(1); return 0; }", "must be an array"));
}

TEST(Frontend, RejectsArrayArgumentForIntParameter) {
  EXPECT_TRUE(rejects("void f(int a) { } int main() { int b[2]; f(b); return 0; }",
                      "must be an int, not an array"));
}

TEST(Frontend, RejectsArrayInitialiser) {
  EXPECT_TRUE(rejects("int main() { int a[3] = 1; return 0; }", "array declaration cannot have"));
}

TEST(Frontend, RejectsZeroSizedArray) {
  EXPECT_TRUE(rejects("int main() { int a[0]; return 0; }", "array size must be at least 1"));
}

// =============================================================================
// Returns and loops
// =============================================================================

TEST(Frontend, RejectsValueReturnFromVoid) {
  EXPECT_TRUE(rejects("void f() { return 1; } int main() { return 0; }",
                      "cannot return a value from void"));
}

TEST(Frontend, RejectsBareReturnFromIntFunction) {
  EXPECT_TRUE(rejects("int f() { return; } int main() { return 0; }", "must return a value"));
}

TEST(Frontend, RejectsBreakOutsideLoop) {
  EXPECT_TRUE(rejects("int main() { break; return 0; }", "'break' outside of a loop"));
}

TEST(Frontend, RejectsContinueOutsideLoop) {
  EXPECT_TRUE(rejects("int main() { continue; return 0; }", "'continue' outside of a loop"));
}

TEST(Frontend, AcceptsBreakInsideNestedBlockInLoop) {
  EXPECT_TRUE(accepts("int main() { while (1) { { break; } } return 0; }"));
}

// =============================================================================
// Syntax errors and recovery
// =============================================================================

TEST(Frontend, RejectsMissingSemicolon) {
  EXPECT_TRUE(rejects("int main() { int x = 1 return 0; }", "expected ';'"));
}

TEST(Frontend, RejectsAssignmentAsExpression) {
  // Spec 5.2: assignment is a statement. `while (x = f())` must not parse.
  EXPECT_TRUE(rejects("int main() { int x; while (x = 1) { } return 0; }", "expected ')'"));
}

TEST(Frontend, RejectsPrintAsExpression) {
  EXPECT_TRUE(rejects("int main() { int x = print(1); return 0; }",
                      "'print' is a statement, not an expression"));
}

TEST(Frontend, RejectsOutOfRangeLiteral) {
  EXPECT_TRUE(rejects("int main() { print(2147483648); return 0; }", "out of range"));
}

TEST(Frontend, ReportsMultipleErrorsInOneRun) {
  // Error recovery: one run should surface both problems, not just the first.
  const std::string errors = errorsFor("int main() { print(a); print(b); return 0; }");
  EXPECT_NE(errors.find("'a'"), std::string::npos) << errors;
  EXPECT_NE(errors.find("'b'"), std::string::npos) << errors;
}

TEST(Frontend, DeeplyNestedParensDoNotCrash) {
  // A hand-written recursive-descent parser will blow the stack on this unless
  // it tracks depth. Must be a diagnostic, not a crash.
  std::string source = "int main() { return ";
  source += std::string(5000, '(');
  source += "1";
  source += std::string(5000, ')');
  source += "; }";
  EXPECT_FALSE(errorsFor(source).empty());
}

TEST(Frontend, UnterminatedBlockIsReported) {
  EXPECT_FALSE(errorsFor("int main() { print(1);").empty());
}

TEST(Frontend, EmptyInputIsRejected) {
  EXPECT_TRUE(rejects("", "no 'main'"));
}
