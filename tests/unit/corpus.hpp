#pragma once

#include <string_view>
#include <vector>

namespace mloptc::test {

/// A named ToyC program used across several test files.
struct CorpusProgram {
  std::string_view name;
  std::string_view source;
};

/// The shared verification corpus.
///
/// Every program here is run through each execution path (AST oracle, IR VM,
/// and LLVM-compiled native code) and through every optimization pipeline. One
/// corpus rather than per-file programs means that adding a program here
/// immediately strengthens every test that uses it -- and that a pass which
/// breaks any language feature is caught, not just one that breaks the feature
/// its own test happened to exercise.
///
/// Programs deliberately include ones that TRAP, because "the optimizer removed
/// a trap" is one of the easiest miscompilations to introduce and one of the
/// hardest to notice.
[[nodiscard]] inline const std::vector<CorpusProgram>& corpus() {
  static const std::vector<CorpusProgram> programs = {
      {"const_fold_chain", R"(
        int main() {
          int a = 2 + 3 * 4 - 1;
          int b = a * 2;
          print(b);
          print(a + b);
          return 0;
        }
      )"},

      {"dead_code", R"(
        int main() {
          int unused = 1 + 2;
          int alsoUnused = unused * 5;
          print(42);
          return 0;
        }
      )"},

      {"copy_chain", R"(
        int main() {
          int a = 7;
          int b = a;
          int c = b;
          int d = c;
          print(d);
          return 0;
        }
      )"},

      {"common_subexpression", R"(
        int main() {
          int x = 3;
          int y = 4;
          int p = x * y + x * y;
          int q = (x + y) * (x + y);
          print(p);
          print(q);
          return 0;
        }
      )"},

      {"fib_recursive", R"(
        int fib(int n) {
          if (n < 2) { return n; }
          return fib(n - 1) + fib(n - 2);
        }
        int main() { print(fib(15)); return 0; }
      )"},

      {"fib_iterative", R"(
        int main() {
          int a = 0;
          int b = 1;
          for (int i = 0; i < 20; i = i + 1) {
            int t = a + b;
            a = b;
            b = t;
          }
          print(a);
          return 0;
        }
      )"},

      {"gcd_loop", R"(
        int gcd(int a, int b) {
          while (b != 0) { int t = b; b = a % b; a = t; }
          return a;
        }
        int main() {
          print(gcd(1071, 462));
          print(gcd(17, 5));
          return 0;
        }
      )"},

      {"sieve", R"(
        int limit = 50;
        int main() {
          int flags[50];
          for (int i = 2; i < limit; i = i + 1) {
            if (flags[i] == 0) {
              print(i);
              for (int j = i + i; j < limit; j = j + i) { flags[j] = 1; }
            }
          }
          return 0;
        }
      )"},

      {"bubble_sort", R"(
        void sort(int a[], int n) {
          for (int i = 0; i < n; i = i + 1) {
            for (int j = 0; j < n - 1 - i; j = j + 1) {
              if (a[j] > a[j + 1]) {
                int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t;
              }
            }
          }
        }
        int main() {
          int a[8];
          a[0] = 5; a[1] = 2; a[2] = 9; a[3] = 1;
          a[4] = 7; a[5] = 3; a[6] = 8; a[7] = 4;
          sort(a, 8);
          for (int i = 0; i < 8; i = i + 1) { print(a[i]); }
          return 0;
        }
      )"},

      {"matrix_like", R"(
        int main() {
          int m[36];
          for (int i = 0; i < 6; i = i + 1) {
            for (int j = 0; j < 6; j = j + 1) {
              m[i * 6 + j] = i * j;
            }
          }
          int total = 0;
          for (int i = 0; i < 36; i = i + 1) { total = total + m[i]; }
          print(total);
          return 0;
        }
      )"},

      {"short_circuit_and", R"(
        int sideEffect(int n) { print(n); return n; }
        int main() {
          if (0 && sideEffect(1)) { print(100); }
          if (1 && sideEffect(2)) { print(200); }
          print(sideEffect(3) && 0);
          return 0;
        }
      )"},

      {"short_circuit_or", R"(
        int sideEffect(int n) { print(n); return n; }
        int main() {
          if (1 || sideEffect(1)) { print(100); }
          if (0 || sideEffect(2)) { print(200); }
          print(0 || sideEffect(0));
          return 0;
        }
      )"},

      {"short_circuit_guards_trap", R"(
        int main() {
          int a[4];
          for (int i = 0; i <= 4; i = i + 1) {
            if (i < 4 && a[i] == 0) { print(i); }
          }
          int d = 0;
          if (d != 0 && 10 / d > 1) { print(999); }
          print(-1);
          return 0;
        }
      )"},

      {"nested_control", R"(
        int main() {
          for (int i = 0; i < 5; i = i + 1) {
            if (i == 2) { continue; }
            for (int j = 0; j < 5; j = j + 1) {
              if (j == 3) { break; }
              if (i + j > 5) { continue; }
              print(i * 10 + j);
            }
          }
          return 0;
        }
      )"},

      {"continue_in_for_runs_update", R"(
        int main() {
          int count = 0;
          for (int i = 0; i < 10; i = i + 1) {
            if (i % 2 == 0) { continue; }
            count = count + i;
          }
          print(count);
          return 0;
        }
      )"},

      {"while_with_break", R"(
        int main() {
          int i = 0;
          int total = 0;
          while (1) {
            if (i >= 10) { break; }
            total = total + i * i;
            i = i + 1;
          }
          print(total);
          return 0;
        }
      )"},

      {"globals_mutated", R"(
        int counter = 100;
        int history[10];
        void bump(int by) {
          history[counter % 10] = counter;
          counter = counter + by;
        }
        int main() {
          for (int i = 1; i <= 5; i = i + 1) { bump(i); }
          print(counter);
          for (int i = 0; i < 10; i = i + 1) { print(history[i]); }
          return 0;
        }
      )"},

      {"mutual_recursion", R"(
        int isEven(int n) { if (n == 0) { return 1; } return isOdd(n - 1); }
        int isOdd(int n) { if (n == 0) { return 0; } return isEven(n - 1); }
        int main() {
          for (int i = 0; i < 8; i = i + 1) { print(isEven(i)); }
          return 0;
        }
      )"},

      {"array_by_reference", R"(
        void fill(int a[], int n, int seed) {
          for (int i = 0; i < n; i = i + 1) { a[i] = (seed + i * 7) % 13; }
        }
        int sum(int a[], int n) {
          int total = 0;
          for (int i = 0; i < n; i = i + 1) { total = total + a[i]; }
          return total;
        }
        int main() {
          int data[12];
          fill(data, 12, 5);
          print(sum(data, 12));
          for (int i = 0; i < 12; i = i + 1) { print(data[i]); }
          return 0;
        }
      )"},

      {"overflow_wraps", R"(
        int main() {
          int big = 2147483647;
          print(big + 1);
          print(big * 2);
          print(-2147483648 - 1);
          int acc = 1;
          for (int i = 0; i < 40; i = i + 1) { acc = acc * 3; }
          print(acc);
          return 0;
        }
      )"},

      {"negative_division", R"(
        int main() {
          print(-7 / 2);
          print(7 / -2);
          print(-7 % 2);
          print(7 % -2);
          print(-100 / 7);
          print(-100 % 7);
          return 0;
        }
      )"},

      {"unused_division_must_still_trap", R"(
        int main() {
          int d = 0;
          print(1);
          int neverRead = 10 / d;
          print(2);
          return 0;
        }
      )"},

      {"unused_array_load_must_still_trap", R"(
        int main() {
          int a[3];
          print(1);
          int neverRead = a[99];
          print(2);
          return 0;
        }
      )"},

      {"trap_division_by_zero", R"(
        int main() {
          print(1);
          print(2);
          int z = 0;
          print(100 / z);
          return 0;
        }
      )"},

      {"trap_out_of_bounds", R"(
        int main() {
          int a[5];
          for (int i = 0; i < 10; i = i + 1) { a[i] = i; print(i); }
          return 0;
        }
      )"},

      {"trap_int_min_div", R"(
        int main() {
          int a = -2147483648;
          int b = -1;
          print(0);
          print(a / b);
          return 0;
        }
      )"},

      {"deep_recursion_traps", R"(
        int f(int n) { return f(n + 1) + 1; }
        int main() { print(1); return f(0); }
      )"},

      {"exit_status_wraps", R"(
        int main() { print(1); return 300; }
      )"},

      {"void_functions", R"(
        int acc = 0;
        void addTo(int n) { acc = acc + n; }
        void addRange(int lo, int hi) {
          for (int i = lo; i < hi; i = i + 1) { addTo(i); }
        }
        int main() { addRange(1, 11); print(acc); return 0; }
      )"},

      {"shadowing", R"(
        int main() {
          int x = 1;
          print(x);
          {
            int x = 2;
            print(x);
            { int x = 3; print(x); }
            print(x);
          }
          print(x);
          return 0;
        }
      )"},

      {"comparison_matrix", R"(
        int main() {
          for (int a = -2; a <= 2; a = a + 1) {
            for (int b = -2; b <= 2; b = b + 1) {
              print(a < b);
              print(a <= b);
              print(a > b);
              print(a >= b);
              print(a == b);
              print(a != b);
            }
          }
          return 0;
        }
      )"},

      {"unary_ops", R"(
        int main() {
          for (int i = -3; i <= 3; i = i + 1) {
            print(-i);
            print(!i);
            print(!!i);
            print(-(-i));
          }
          return 0;
        }
      )"},

      {"empty_main", R"(
        int main() { return 0; }
      )"},

      {"missing_return_is_zero", R"(
        int f(int n) { if (n > 100) { return 1; } }
        int main() { print(f(1)); print(f(200)); return 0; }
      )"},

      {"many_blocks", R"(
        int classify(int n) {
          if (n < 0) { return 0; }
          if (n == 0) { return 1; }
          if (n < 10) { return 2; }
          if (n < 100) { return 3; }
          if (n % 2 == 0) { return 4; }
          return 5;
        }
        int main() {
          int xs[7];
          xs[0] = -5; xs[1] = 0; xs[2] = 3; xs[3] = 42;
          xs[4] = 101; xs[5] = 1000; xs[6] = 7;
          for (int i = 0; i < 7; i = i + 1) { print(classify(xs[i])); }
          return 0;
        }
      )"},
  };
  return programs;
}

} // namespace mloptc::test
