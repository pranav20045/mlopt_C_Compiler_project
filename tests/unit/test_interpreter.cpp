#include "mloptc/driver.hpp"
#include "mloptc/interpreter.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace mloptc;

namespace {

/// Compiles and runs a program, asserting that the front end accepted it.
ExecutionResult run(std::string_view source) {
  DiagnosticEngine diags;
  auto program = compileFrontend(source, diags);
  if (!program) {
    std::string messages;
    for (const Diagnostic& d : diags.all()) {
      messages += d.message + "; ";
    }
    ADD_FAILURE() << "front end rejected the program: " << messages;
    return {};
  }
  return interpret(*program);
}

/// Wraps a body in `int main() { ... }` to keep tests short.
std::string inMain(std::string_view body) {
  return "int main() {\n" + std::string(body) + "\n return 0; }";
}

std::string runMain(std::string_view body) {
  return run(inMain(body)).output;
}

} // namespace

// =============================================================================
// Arithmetic
// =============================================================================

TEST(Interp, BasicArithmetic) {
  EXPECT_EQ(runMain("print(1 + 2);"), "3\n");
  EXPECT_EQ(runMain("print(10 - 4);"), "6\n");
  EXPECT_EQ(runMain("print(6 * 7);"), "42\n");
  EXPECT_EQ(runMain("print(20 / 3);"), "6\n");
  EXPECT_EQ(runMain("print(20 % 3);"), "2\n");
}

TEST(Interp, OperatorPrecedence) {
  EXPECT_EQ(runMain("print(2 + 3 * 4);"), "14\n");
  EXPECT_EQ(runMain("print((2 + 3) * 4);"), "20\n");
  EXPECT_EQ(runMain("print(2 * 3 + 4 * 5);"), "26\n");
  EXPECT_EQ(runMain("print(1 - 2 - 3);"), "-4\n"); // left-associative
  EXPECT_EQ(runMain("print(100 / 10 / 2);"), "5\n");
}

TEST(Interp, UnaryOperators) {
  EXPECT_EQ(runMain("print(-5);"), "-5\n");
  EXPECT_EQ(runMain("print(--5);"), "5\n");
  EXPECT_EQ(runMain("print(!0);"), "1\n");
  EXPECT_EQ(runMain("print(!5);"), "0\n");
  EXPECT_EQ(runMain("print(!!7);"), "1\n");
}

TEST(Interp, TruncatingDivisionMatchesSpec) {
  // Spec 7.2: division truncates toward zero; the remainder takes the sign of
  // the dividend.
  EXPECT_EQ(runMain("print(-7 / 2);"), "-3\n");
  EXPECT_EQ(runMain("print(7 / -2);"), "-3\n");
  EXPECT_EQ(runMain("print(-7 % 2);"), "-1\n");
  EXPECT_EQ(runMain("print(7 % -2);"), "1\n");
}

TEST(Interp, IntegerOverflowWraps) {
  // Spec 7.1. In C this would be UB; here it is pinned down.
  EXPECT_EQ(runMain("print(2147483647 + 1);"), "-2147483648\n");
  EXPECT_EQ(runMain("print(-2147483648 - 1);"), "2147483647\n");
  EXPECT_EQ(runMain("print(2147483647 * 2);"), "-2\n");
  EXPECT_EQ(runMain("print(-(-2147483648));"), "-2147483648\n");
}

TEST(Interp, IntMinLiteralIsAccepted) {
  EXPECT_EQ(runMain("print(-2147483648);"), "-2147483648\n");
}

// =============================================================================
// Comparison and logic
// =============================================================================

TEST(Interp, ComparisonsYieldZeroOrOne) {
  EXPECT_EQ(runMain("print(3 < 5);"), "1\n");
  EXPECT_EQ(runMain("print(5 < 3);"), "0\n");
  EXPECT_EQ(runMain("print(3 <= 3);"), "1\n");
  EXPECT_EQ(runMain("print(4 > 2);"), "1\n");
  EXPECT_EQ(runMain("print(4 >= 5);"), "0\n");
  EXPECT_EQ(runMain("print(2 == 2);"), "1\n");
  EXPECT_EQ(runMain("print(2 != 2);"), "0\n");
}

TEST(Interp, LogicalOperatorsNormaliseToZeroOrOne) {
  EXPECT_EQ(runMain("print(5 && 3);"), "1\n");
  EXPECT_EQ(runMain("print(5 || 3);"), "1\n");
  EXPECT_EQ(runMain("print(0 && 3);"), "0\n");
  EXPECT_EQ(runMain("print(0 || 0);"), "0\n");
}

TEST(Interp, AndShortCircuits) {
  // If && did not short-circuit, side() would run and print 99.
  const auto out = run(R"(
    int side() { print(99); return 1; }
    int main() {
      if (0 && side()) { print(1); }
      print(7);
      return 0;
    }
  )")
                       .output;
  EXPECT_EQ(out, "7\n");
}

TEST(Interp, OrShortCircuits) {
  const auto out = run(R"(
    int side() { print(99); return 1; }
    int main() {
      if (1 || side()) { print(1); }
      return 0;
    }
  )")
                       .output;
  EXPECT_EQ(out, "1\n");
}

TEST(Interp, ShortCircuitGuardsAgainstTraps) {
  // The classic reason short-circuiting is a correctness feature and not just
  // an optimization: without it, a[10] would be evaluated and trap.
  const auto result = run(R"(
    int main() {
      int a[10];
      int i = 10;
      if (i < 10 && a[i] == 0) { print(1); }
      print(2);
      return 0;
    }
  )");
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.output, "2\n");
}

// =============================================================================
// Control flow
// =============================================================================

TEST(Interp, IfElse) {
  EXPECT_EQ(runMain("if (1) { print(1); } else { print(2); }"), "1\n");
  EXPECT_EQ(runMain("if (0) { print(1); } else { print(2); }"), "2\n");
  EXPECT_EQ(runMain("if (0) { print(1); }"), "");
}

TEST(Interp, DanglingElseBindsToNearestIf) {
  // if (1) { if (0) print(1); else print(2); }  -- the else is the inner one,
  // so this prints 2. If it bound to the outer if, nothing would print.
  EXPECT_EQ(runMain("if (1) if (0) print(1); else print(2);"), "2\n");
}

TEST(Interp, WhileLoop) {
  EXPECT_EQ(runMain("int i = 0; while (i < 3) { print(i); i = i + 1; }"), "0\n1\n2\n");
}

TEST(Interp, ForLoop) {
  EXPECT_EQ(runMain("for (int i = 0; i < 3; i = i + 1) { print(i); }"), "0\n1\n2\n");
}

TEST(Interp, ForLoopWithoutCondition) {
  EXPECT_EQ(runMain("for (int i = 0; ; i = i + 1) { if (i > 2) break; print(i); }"), "0\n1\n2\n");
}

TEST(Interp, BreakExitsLoop) {
  EXPECT_EQ(runMain("int i = 0; while (1) { if (i == 2) break; print(i); i = i + 1; }"), "0\n1\n");
}

TEST(Interp, ContinueSkipsRestOfBody) {
  EXPECT_EQ(runMain("int i = 0; while (i < 4) { i = i + 1; if (i == 2) continue; print(i); }"),
            "1\n3\n4\n");
}

TEST(Interp, ContinueInForStillRunsUpdate) {
  // THE regression test for the `for` desugaring trap. If `continue` skipped
  // the update, i would never advance past 1 and this would hang until the
  // step limit.
  const auto result = run(inMain("for (int i = 0; i < 4; i = i + 1) { "
                                 "  if (i == 1) continue; print(i); }"));
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.output, "0\n2\n3\n");
}

TEST(Interp, NestedLoopsBreakInnermostOnly) {
  EXPECT_EQ(runMain(R"(
    for (int i = 0; i < 2; i = i + 1) {
      for (int j = 0; j < 5; j = j + 1) {
        if (j == 2) break;
        print(i * 10 + j);
      }
    }
  )"),
            "0\n1\n10\n11\n");
}

// =============================================================================
// Functions
// =============================================================================

TEST(Interp, FunctionCallAndReturn) {
  EXPECT_EQ(run(R"(
    int add(int a, int b) { return a + b; }
    int main() { print(add(3, 4)); return 0; }
  )")
                .output,
            "7\n");
}

TEST(Interp, Recursion) {
  EXPECT_EQ(run(R"(
    int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
    int main() { print(fib(10)); return 0; }
  )")
                .output,
            "55\n");
}

TEST(Interp, MutualRecursionWithoutForwardDeclaration) {
  // Functions are visible throughout the file, so isEven can call isOdd even
  // though isOdd is defined later.
  EXPECT_EQ(run(R"(
    int isEven(int n) { if (n == 0) return 1; return isOdd(n - 1); }
    int isOdd(int n) { if (n == 0) return 0; return isEven(n - 1); }
    int main() { print(isEven(10)); print(isOdd(10)); return 0; }
  )")
                .output,
            "1\n0\n");
}

TEST(Interp, VoidFunction) {
  EXPECT_EQ(run(R"(
    void greet(int n) { print(n); }
    int main() { greet(5); return 0; }
  )")
                .output,
            "5\n");
}

TEST(Interp, MissingReturnYieldsZero) {
  // Spec 5.4: falling off the end of an int function returns 0, where C has UB.
  EXPECT_EQ(run(R"(
    int f(int n) { if (n > 100) return 1; }
    int main() { print(f(1)); return 0; }
  )")
                .output,
            "0\n");
}

TEST(Interp, ArgumentsEvaluateLeftToRight) {
  // Unspecified in C; pinned down by spec section 6.
  EXPECT_EQ(run(R"(
    int t(int n) { print(n); return n; }
    int f(int a, int b, int c) { return 0; }
    int main() { f(t(1), t(2), t(3)); return 0; }
  )")
                .output,
            "1\n2\n3\n");
}

TEST(Interp, RecursionDepthIsBounded) {
  // Spec 7.5: this must be a clean trap, not a stack overflow crash.
  const auto result = run(R"(
    int f(int n) { return f(n + 1); }
    int main() { return f(0); }
  )");
  EXPECT_EQ(result.status, RuntimeStatus::CallDepthExceeded);
  EXPECT_EQ(result.exitStatus, 70);
}

// =============================================================================
// Variables and scope
// =============================================================================

TEST(Interp, UninitialisedLocalIsZero) {
  EXPECT_EQ(runMain("int x; print(x);"), "0\n");
}

TEST(Interp, GlobalsPersistAcrossCalls) {
  EXPECT_EQ(run(R"(
    int counter = 10;
    void bump() { counter = counter + 1; }
    int main() { bump(); bump(); print(counter); return 0; }
  )")
                .output,
            "12\n");
}

TEST(Interp, UninitialisedGlobalIsZero) {
  EXPECT_EQ(run("int g; int main() { print(g); return 0; }").output, "0\n");
}

TEST(Interp, NegativeGlobalInitialiser) {
  EXPECT_EQ(run("int g = -42; int main() { print(g); return 0; }").output, "-42\n");
}

TEST(Interp, InnerScopeShadowsOuter) {
  EXPECT_EQ(runMain("int x = 1; { int x = 2; print(x); } print(x);"), "2\n1\n");
}

TEST(Interp, LocalsAreFreshPerCall) {
  EXPECT_EQ(run(R"(
    int f(int n) { int x = n * 2; return x; }
    int main() { print(f(1)); print(f(2)); return 0; }
  )")
                .output,
            "2\n4\n");
}

// =============================================================================
// Arrays
// =============================================================================

TEST(Interp, ArrayElementsStartAtZero) {
  EXPECT_EQ(runMain("int a[3]; print(a[0]); print(a[1]); print(a[2]);"), "0\n0\n0\n");
}

TEST(Interp, ArrayReadWrite) {
  EXPECT_EQ(runMain("int a[3]; a[0] = 7; a[2] = 9; print(a[0]); print(a[2]);"), "7\n9\n");
}

TEST(Interp, ArrayIndexedByExpression) {
  EXPECT_EQ(runMain("int a[5]; int i = 1; a[i + 1] = 42; print(a[2]);"), "42\n");
}

TEST(Interp, GlobalArray) {
  EXPECT_EQ(run(R"(
    int g[4];
    int main() { g[1] = 5; print(g[1]); print(g[0]); return 0; }
  )")
                .output,
            "5\n0\n");
}

TEST(Interp, ArrayParameterIsByReference) {
  // Spec 4.2: the only reference semantics in the language.
  EXPECT_EQ(run(R"(
    void fill(int a[], int n) { for (int i = 0; i < n; i = i + 1) { a[i] = i * i; } }
    int main() {
      int data[4];
      fill(data, 4);
      for (int i = 0; i < 4; i = i + 1) { print(data[i]); }
      return 0;
    }
  )")
                .output,
            "0\n1\n4\n9\n");
}

TEST(Interp, ArrayOutOfBoundsTrapsHigh) {
  const auto result = run(inMain("int a[3]; print(a[3]);"));
  EXPECT_EQ(result.status, RuntimeStatus::IndexOutOfBounds);
  EXPECT_EQ(result.exitStatus, 70);
}

TEST(Interp, ArrayOutOfBoundsTrapsNegative) {
  const auto result = run(inMain("int a[3]; print(a[-1]);"));
  EXPECT_EQ(result.status, RuntimeStatus::IndexOutOfBounds);
}

TEST(Interp, OutputBeforeTrapIsRetained) {
  // Spec 7.6: stdout produced before the error must still be observable, and
  // must match across backends.
  const auto result = run(inMain("print(1); int a[2]; print(a[5]);"));
  EXPECT_EQ(result.output, "1\n");
  EXPECT_EQ(result.exitStatus, 70);
}

// =============================================================================
// Traps
// =============================================================================

TEST(Interp, DivisionByZeroTraps) {
  const auto result = run(inMain("int z = 0; print(1 / z);"));
  EXPECT_EQ(result.status, RuntimeStatus::DivisionByZero);
  EXPECT_EQ(result.exitStatus, 70);
}

TEST(Interp, RemainderByZeroTraps) {
  const auto result = run(inMain("int z = 0; print(1 % z);"));
  EXPECT_EQ(result.status, RuntimeStatus::RemainderByZero);
}

TEST(Interp, IntMinDividedByMinusOneTraps) {
  // Spec 7.2: would overflow, and faults on real x86 hardware.
  const auto result = run(inMain("int a = -2147483648; int b = -1; print(a / b);"));
  EXPECT_EQ(result.status, RuntimeStatus::DivisionOverflow);
}

TEST(Interp, IntMinRemainderMinusOneTraps) {
  const auto result = run(inMain("int a = -2147483648; int b = -1; print(a % b);"));
  EXPECT_EQ(result.status, RuntimeStatus::DivisionOverflow);
}

// =============================================================================
// Exit status
// =============================================================================

TEST(Interp, ExitStatusIsMainReturnValue) {
  EXPECT_EQ(run("int main() { return 42; }").exitStatus, 42);
}

TEST(Interp, ExitStatusIsReducedModulo256) {
  // Spec section 8.
  EXPECT_EQ(run("int main() { return 300; }").exitStatus, 44);
  EXPECT_EQ(run("int main() { return -1; }").exitStatus, 255);
  EXPECT_EQ(run("int main() { return 256; }").exitStatus, 0);
}

// =============================================================================
// Larger programs
// =============================================================================

TEST(Interp, GcdAndSieve) {
  const auto result = run(R"(
    int gcd(int a, int b) {
      while (b != 0) { int t = b; b = a % b; a = t; }
      return a;
    }
    int limit = 20;
    int main() {
      print(gcd(48, 18));
      int primes[20];
      int i = 2;
      while (i < limit) {
        if (primes[i] == 0) {
          print(i);
          for (int j = i + i; j < limit; j = j + i) { primes[j] = 1; }
        }
        i = i + 1;
      }
      return 0;
    }
  )");
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.output, "6\n2\n3\n5\n7\n11\n13\n17\n19\n");
}

TEST(Interp, BubbleSort) {
  const auto result = run(R"(
    void sort(int a[], int n) {
      for (int i = 0; i < n; i = i + 1) {
        for (int j = 0; j < n - 1 - i; j = j + 1) {
          if (a[j] > a[j + 1]) {
            int t = a[j];
            a[j] = a[j + 1];
            a[j + 1] = t;
          }
        }
      }
    }
    int main() {
      int a[6];
      a[0] = 5; a[1] = 2; a[2] = 9; a[3] = 1; a[4] = 7; a[5] = 3;
      sort(a, 6);
      for (int i = 0; i < 6; i = i + 1) { print(a[i]); }
      return 0;
    }
  )");
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.output, "1\n2\n3\n5\n7\n9\n");
}
