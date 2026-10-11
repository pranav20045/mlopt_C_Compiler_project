"""A random ToyC program generator.

The generator's job is to produce programs that are *interesting* (they exercise
loops, arrays, recursion, overflow, short-circuiting) while being well-formed --
so that any disagreement between backends is a real compiler bug rather than a
rejected program.

Note what it does NOT have to avoid. Because ToyC is specified to have no
undefined behaviour (docs/spec.md section 1), the generator is free to emit
division by zero, out-of-bounds indices and signed overflow: all three have
exactly one defined outcome, so all three backends must agree on them. That is
the payoff for defining the language tightly, and it is what lets trapping
programs be a deliberate part of the test distribution rather than something to
filter out. Generating UB-free *C* would be far harder.

Two properties the generator does have to guarantee, because they are
compile-time rules rather than runtime behaviour:

  * every name is declared before use, with the right type (scalar vs array)
  * every call matches its callee's arity and parameter kinds

Those are enforced by construction: the generator tracks the scope it is
emitting into and only ever picks a name that is already in it.
"""

from __future__ import annotations

import argparse
import random
import sys

#: Guards against a generated program that loops for a very long time. Loop
#: bounds are kept small so a random program finishes quickly -- the fuzzer's
#: throughput matters more than any individual program's depth.
MAX_LOOP_TRIPS = 40


class Scope:
    """Names visible at one point, split by kind."""

    def __init__(self, scalars: list[str] | None = None, arrays: list[tuple[str, int]] | None = None):
        self.scalars: list[str] = list(scalars or [])
        #: (name, length). Length is tracked so the generator can bias indices
        #: toward the valid range without being unable to go outside it.
        self.arrays: list[tuple[str, int]] = list(arrays or [])

    def copy(self) -> "Scope":
        return Scope(self.scalars, self.arrays)


class ProgramGenerator:
    def __init__(self, rng: random.Random, *, max_depth: int = 3, size: str = "medium"):
        self.rng = rng
        self.max_depth = max_depth
        self.size = size
        self._counter = 0

        #: name -> (param_kinds, returns_value), filled as functions are made.
        #: Only functions already generated can be called, which makes
        #: arity and parameter kinds correct by construction.
        self.functions: dict[str, tuple[list[str], bool]] = {}
        #: Variables that control a loop. Off limits as assignment targets.
        self.loop_counters: set[str] = set()
        self.global_scalars: list[str] = []
        self.global_arrays: list[tuple[str, int]] = []

    # -- helpers --------------------------------------------------------------

    def fresh(self, prefix: str) -> str:
        self._counter += 1
        return f"{prefix}{self._counter}"

    def literal(self) -> str:
        """A constant, biased toward values that stress the implementation.

        Small values dominate because they keep loops terminating, but the
        extremes are included deliberately: INT_MIN and INT_MAX are where
        wrapping and the INT_MIN / -1 trap live, and zero is where the division
        traps live. A generator that only produced "ordinary" numbers would
        never test any of them.
        """
        roll = self.rng.random()
        if roll < 0.08:
            return self.rng.choice(["2147483647", "-2147483648", "0", "1", "-1"])
        if roll < 0.20:
            return str(self.rng.randint(-1000, 1000))
        return str(self.rng.randint(-20, 20))

    # -- expressions ----------------------------------------------------------

    def expr(self, scope: Scope, depth: int = 0) -> str:
        if depth >= self.max_depth or self.rng.random() < 0.3:
            return self.atom(scope)

        choice = self.rng.random()

        if choice < 0.40:
            op = self.rng.choice(["+", "-", "*", "/", "%"])
            left = self.expr(scope, depth + 1)
            right = self.expr(scope, depth + 1)
            return f"({left} {op} {right})"

        if choice < 0.60:
            op = self.rng.choice(["<", "<=", ">", ">=", "==", "!="])
            return f"({self.expr(scope, depth + 1)} {op} {self.expr(scope, depth + 1)})"

        if choice < 0.75:
            # Short-circuit operators: worth generating often, since they are
            # the only place ToyC has control flow inside an expression and the
            # only place an operand may legitimately go unevaluated.
            op = self.rng.choice(["&&", "||"])
            return f"({self.expr(scope, depth + 1)} {op} {self.expr(scope, depth + 1)})"

        if choice < 0.85:
            op = self.rng.choice(["-", "!"])
            return f"{op}({self.expr(scope, depth + 1)})"

        if choice < 0.95 and scope.arrays:
            name, length = self.rng.choice(scope.arrays)
            return f"{name}[{self.index_expr(scope, length, depth)}]"

        # can_call matters: a function with an array parameter can only be
        # called where an array is actually in scope to pass to it.
        callable_fns = [
            n for n, (_, returns) in self.functions.items() if returns and self.can_call(scope, n)
        ]
        if callable_fns:
            name = self.rng.choice(callable_fns)
            return f"{name}({self.call_args(scope, name, depth)})"

        return self.atom(scope)

    def atom(self, scope: Scope) -> str:
        pool: list[str] = [self.literal()]
        pool += scope.scalars
        pool += self.global_scalars
        return self.rng.choice(pool)

    def index_expr(self, scope: Scope, length: int, depth: int) -> str:
        """An array index.

        Mostly in range, so programs usually run to completion and exercise the
        optimizer -- but 10% of the time deliberately out of range, because the
        bounds-check trap is observable behaviour that every backend must agree
        on, and a generator that never produced one would leave that untested.
        """
        roll = self.rng.random()
        if roll < 0.10:
            return self.rng.choice([str(length), str(length + 5), "-1", self.expr(scope, depth + 1)])
        if roll < 0.40 and scope.scalars:
            # A computed index, kept in range with %.
            return f"(({self.rng.choice(scope.scalars)} % {length}) + {length}) % {length}"
        return str(self.rng.randrange(length))

    def call_args(self, scope: Scope, name: str, depth: int) -> str:
        kinds, _ = self.functions[name]
        parts: list[str] = []
        for kind in kinds:
            if kind == "array":
                pool = scope.arrays + self.global_arrays
                parts.append(self.rng.choice(pool)[0])
            else:
                parts.append(self.expr(scope, depth + 1))
        return ", ".join(parts)

    def can_call(self, scope: Scope, name: str) -> bool:
        """An array parameter needs an array in scope to pass."""
        kinds, _ = self.functions[name]
        if "array" in kinds and not (scope.arrays or self.global_arrays):
            return False
        return True

    # -- statements -----------------------------------------------------------

    def statement(self, scope: Scope, depth: int, in_loop: bool, lines: list[str], indent: str) -> None:
        roll = self.rng.random()

        if roll < 0.18:
            name = self.fresh("v")
            lines.append(f"{indent}int {name} = {self.expr(scope)};")
            scope.scalars.append(name)
            return

        if roll < 0.34:
            # Never assign to a loop counter. The generated loop bound is what
            # guarantees termination, and a body that reassigns the counter
            # takes that guarantee away -- producing programs that run until
            # some backend's step limit, which is noise rather than signal.
            assignable = [v for v in scope.scalars if v not in self.loop_counters]
            if assignable:
                target = self.rng.choice(assignable)
                lines.append(f"{indent}{target} = {self.expr(scope)};")
                return

        if roll < 0.46:
            lines.append(f"{indent}print({self.expr(scope)});")
            return

        if roll < 0.56 and scope.arrays:
            name, length = self.rng.choice(scope.arrays)
            lines.append(f"{indent}{name}[{self.index_expr(scope, length, 0)}] = {self.expr(scope)};")
            return

        if roll < 0.62 and self.global_scalars:
            target = self.rng.choice(self.global_scalars)
            lines.append(f"{indent}{target} = {self.expr(scope)};")
            return

        if roll < 0.74 and depth < self.max_depth:
            lines.append(f"{indent}if ({self.expr(scope)}) {{")
            self.block(scope.copy(), depth + 1, in_loop, lines, indent + "  ")
            if self.rng.random() < 0.5:
                lines.append(f"{indent}}} else {{")
                self.block(scope.copy(), depth + 1, in_loop, lines, indent + "  ")
            lines.append(f"{indent}}}")
            return

        if roll < 0.88 and depth < self.max_depth:
            self.loop(scope, depth, lines, indent)
            return

        if roll < 0.93 and in_loop:
            lines.append(f"{indent}{self.rng.choice(['break', 'continue'])};")
            return

        callable_fns = [n for n in self.functions if self.can_call(scope, n)]
        if callable_fns:
            name = self.rng.choice(callable_fns)
            _, returns = self.functions[name]
            call = f"{name}({self.call_args(scope, name, 0)})"
            lines.append(f"{indent}{'print(' + call + ');' if returns else call + ';'}")
            return

        lines.append(f"{indent}print({self.expr(scope)});")

    def loop(self, scope: Scope, depth: int, lines: list[str], indent: str) -> None:
        """A loop with a bounded trip count.

        The counter is always a fresh variable incremented by a positive
        constant and compared against a fixed bound, so the loop terminates no
        matter what the body does to other variables. Unbounded loops are legal
        ToyC, but a fuzzer full of them just burns timeouts.
        """
        var = self.fresh("i")
        trips = self.rng.randint(1, MAX_LOOP_TRIPS)
        self.loop_counters.add(var)
        body_scope = scope.copy()
        body_scope.scalars.append(var)

        if self.rng.random() < 0.6:
            step = self.rng.randint(1, 3)
            lines.append(f"{indent}for (int {var} = 0; {var} < {trips}; {var} = {var} + {step}) {{")
            self.block(body_scope, depth + 1, True, lines, indent + "  ")
            lines.append(f"{indent}}}")
        else:
            lines.append(f"{indent}int {var} = 0;")
            scope.scalars.append(var)
            lines.append(f"{indent}while ({var} < {trips}) {{")
            inner = scope.copy()
            # The increment is emitted FIRST so that every path through the body
            # advances the counter -- otherwise a generated `continue` would
            # skip it and hang.
            lines.append(f"{indent}  {var} = {var} + 1;")
            self.block(inner, depth + 1, True, lines, indent + "  ")
            lines.append(f"{indent}}}")

    def block(self, scope: Scope, depth: int, in_loop: bool, lines: list[str], indent: str) -> None:
        count = self.rng.randint(1, 4 if depth > 1 else 6)
        for _ in range(count):
            self.statement(scope, depth, in_loop, lines, indent)

    # -- declarations ---------------------------------------------------------

    def make_function(self, name: str, *, returns: bool) -> list[str]:
        param_kinds: list[str] = []
        params: list[str] = []
        scope = Scope()

        for _ in range(self.rng.randint(0, 3)):
            if self.rng.random() < 0.25:
                pname = self.fresh("a")
                params.append(f"int {pname}[]")
                param_kinds.append("array")
                # The length of an array parameter is unknown inside the callee,
                # so a conservative small bound is assumed for index generation.
                scope.arrays.append((pname, 4))
            else:
                pname = self.fresh("p")
                params.append(f"int {pname}")
                param_kinds.append("int")
                scope.scalars.append(pname)

        # Registered BEFORE the body is generated, so the function may call
        # itself -- recursion is a feature worth fuzzing.
        self.functions[name] = (param_kinds, returns)

        lines = [f"{'int' if returns else 'void'} {name}({', '.join(params)}) {{"]

        for _ in range(self.rng.randint(0, 2)):
            arr = self.fresh("arr")
            length = self.rng.randint(1, 12)
            lines.append(f"  int {arr}[{length}];")
            scope.arrays.append((arr, length))

        self.block(scope, 1, False, lines, "  ")

        if returns:
            lines.append(f"  return {self.expr(scope)};")
        lines.append("}")
        return lines

    def generate(self) -> str:
        counts = {"small": (0, 1), "medium": (1, 3), "large": (2, 5)}
        low, high = counts.get(self.size, (1, 3))

        out: list[str] = []

        for _ in range(self.rng.randint(0, 2)):
            name = self.fresh("g")
            out.append(f"int {name} = {self.rng.randint(-100, 100)};")
            self.global_scalars.append(name)

        if self.rng.random() < 0.5:
            name = self.fresh("ga")
            length = self.rng.randint(1, 16)
            out.append(f"int {name}[{length}];")
            self.global_arrays.append((name, length))

        if out:
            out.append("")

        for _ in range(self.rng.randint(low, high)):
            out.extend(self.make_function(self.fresh("f"), returns=self.rng.random() < 0.7))
            out.append("")

        # `main` is generated last so it can call everything else.
        main_scope = Scope()
        main_lines = ["int main() {"]
        for _ in range(self.rng.randint(0, 2)):
            arr = self.fresh("arr")
            length = self.rng.randint(1, 12)
            main_lines.append(f"  int {arr}[{length}];")
            main_scope.arrays.append((arr, length))

        self.block(main_scope, 1, False, main_lines, "  ")
        main_lines.append(f"  return {self.expr(main_scope)};")
        main_lines.append("}")

        out.extend(main_lines)
        return "\n".join(out) + "\n"


def generate_program(seed: int, *, size: str = "medium", max_depth: int = 3) -> str:
    """Generates one program. The same seed always gives the same program,
    which is what makes a fuzzer failure reproducible from its seed alone."""
    generator = ProgramGenerator(random.Random(seed), max_depth=max_depth, size=size)
    return generator.generate()


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a random ToyC program.")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--size", choices=["small", "medium", "large"], default="medium")
    parser.add_argument("--max-depth", type=int, default=3)
    parser.add_argument("--count", type=int, default=1)
    args = parser.parse_args()

    for i in range(args.count):
        if args.count > 1:
            print(f"// ---- seed {args.seed + i} ----")
        sys.stdout.write(generate_program(args.seed + i, size=args.size, max_depth=args.max_depth))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
