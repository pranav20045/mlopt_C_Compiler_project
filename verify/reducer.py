"""A test-case reducer for ToyC programs.

When the fuzzer finds a mismatch, the program it found it in is usually 50+
lines of generated noise, of which two lines matter. Debugging that by hand is
miserable and slow. The reducer repeatedly deletes parts of the program and
keeps any deletion that still reproduces the mismatch, until nothing more can
be removed -- the same idea as C-Reduce or `git bisect`, applied to source text.

The "interestingness test" is deliberately strict: a reduction is only kept if
the program still COMPILES and still shows the SAME KIND of disagreement.
Without that, the reducer happily "reduces" a wrong-answer bug into a syntax
error, which reproduces nothing.

Usage:
    python verify/reducer.py --seed 1234            # reduce a fuzzer finding
    python verify/reducer.py --file broken.tc       # reduce a specific file
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from verify.generator import generate_program  # noqa: E402
from verify.toolchain import Toolchain, ToolchainError  # noqa: E402

DEFAULT_PIPELINE = ["loadelim", "copyprop", "constfold", "cse", "copyprop", "dce"]


class InterestingnessTest:
    """Decides whether a candidate program still exhibits the bug."""

    def __init__(self, tc: Toolchain, *, include_native: bool, pipeline: list[str]):
        self.tc = tc
        self.include_native = include_native and tc.has_native
        self.pipeline = pipeline
        self.calls = 0

    def __call__(self, source: str) -> bool:
        self.calls += 1
        with tempfile.TemporaryDirectory(prefix="mloptc_reduce_") as tmp:
            path = pathlib.Path(tmp) / "program.tc"
            path.write_text(source, encoding="utf-8")

            # A program that no longer compiles is not a smaller reproduction,
            # it is a different (and uninteresting) program.
            if not self.tc.compiles(path):
                return False

            oracle = self.tc.run_oracle(path)
            if oracle.timed_out:
                return False

            candidates = [self.tc.run_vm(path, None), self.tc.run_vm(path, self.pipeline)]
            if self.include_native:
                try:
                    candidates.append(self.tc.run_native(path, None))
                    candidates.append(self.tc.run_native(path, self.pipeline))
                except ToolchainError:
                    return False

            return any(not oracle.observably_equals(other) for other in candidates)


def _lines(source: str) -> list[str]:
    return source.splitlines()


def _join(lines: list[str]) -> str:
    return "\n".join(lines) + "\n"


def _balanced(lines: list[str]) -> bool:
    """Cheap pre-filter: skip candidates whose braces obviously do not match.

    Compiling is by far the slowest step, so rejecting hopeless candidates with
    a brace count first is worth the few lines it costs.
    """
    depth = 0
    for line in lines:
        depth += line.count("{") - line.count("}")
        if depth < 0:
            return False
    return depth == 0


def reduce_by_lines(source: str, interesting, *, verbose: bool = True) -> str:
    """Delta debugging over lines, with a shrinking chunk size.

    Starting with large chunks removes whole functions and loop bodies quickly;
    narrowing to single lines at the end does the fine work. Going straight to
    single lines would take far more compiles to reach the same place.
    """
    lines = _lines(source)
    chunk = max(1, len(lines) // 2)

    while chunk >= 1:
        index = 0
        while index < len(lines):
            candidate = lines[:index] + lines[index + chunk :]
            if candidate and _balanced(candidate) and interesting(_join(candidate)):
                lines = candidate
                if verbose:
                    print(f"  removed {chunk} line(s) at {index}: now {len(lines)} lines")
                # Do not advance: the next chunk has shifted into this position.
                continue
            index += 1
        chunk //= 2

    return _join(lines)


def reduce_by_simplification(source: str, interesting, *, verbose: bool = True) -> str:
    """Replaces complicated expressions with simpler ones.

    Line deletion alone leaves lines like
        print(((a % b) / (c != d)) + e);
    intact. These rewrites shrink what remains, which is usually what turns a
    reproduction into an explanation.
    """
    rewrites: list[tuple[str, str]] = [
        # Parenthesised subexpressions -> 0, innermost first.
        (r"\(\s*-?\d+\s*[-+*/%]\s*-?\d+\s*\)", "0"),
        (r"\(\s*[A-Za-z_]\w*\s*[-+*/%<>=!&|]+\s*-?\d+\s*\)", "0"),
        # Large literals -> small ones (keeps the shape, loses the noise).
        (r"\b\d{4,}\b", "1"),
    ]

    current = source
    changed = True
    while changed:
        changed = False
        for pattern, replacement in rewrites:
            for match in list(re.finditer(pattern, current)):
                candidate = current[: match.start()] + replacement + current[match.end() :]
                if interesting(candidate):
                    current = candidate
                    changed = True
                    if verbose:
                        print(f"  simplified {match.group()!r} -> {replacement!r}")
                    break
            if changed:
                break

    return current


def reduce_program(
    source: str,
    tc: Toolchain,
    *,
    include_native: bool = True,
    pipeline: list[str] | None = None,
    verbose: bool = True,
) -> tuple[str, int]:
    """Reduces `source` as far as it can. Returns (reduced source, test count)."""
    interesting = InterestingnessTest(
        tc, include_native=include_native, pipeline=pipeline or DEFAULT_PIPELINE
    )

    if not interesting(source):
        raise ValueError("the input program does not reproduce any mismatch")

    if verbose:
        print(f"starting from {len(_lines(source))} lines")

    current = source
    # Alternate the two strategies: deleting lines exposes new expressions to
    # simplify, and simplifying expressions can make whole lines removable.
    for round_number in range(1, 4):
        if verbose:
            print(f"round {round_number}: deleting lines")
        after_lines = reduce_by_lines(current, interesting, verbose=verbose)

        if verbose:
            print(f"round {round_number}: simplifying expressions")
        after_simplify = reduce_by_simplification(after_lines, interesting, verbose=verbose)

        if after_simplify == current:
            break
        current = after_simplify

    return current, interesting.calls


def main() -> int:
    parser = argparse.ArgumentParser(description="Reduce a failing ToyC program.")
    source_group = parser.add_mutually_exclusive_group(required=True)
    source_group.add_argument("--seed", type=int, help="regenerate the program from this seed")
    source_group.add_argument("--file", type=pathlib.Path, help="reduce this file")
    parser.add_argument("--size", choices=["small", "medium", "large"], default="medium")
    parser.add_argument("--max-depth", type=int, default=3)
    parser.add_argument("--no-native", action="store_true")
    parser.add_argument("--output", type=pathlib.Path, help="write the reduced program here")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    try:
        tc = Toolchain.discover()
    except ToolchainError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    source = (
        args.file.read_text(encoding="utf-8")
        if args.file
        else generate_program(args.seed, size=args.size, max_depth=args.max_depth)
    )

    try:
        reduced, calls = reduce_program(
            source, tc, include_native=not args.no_native, verbose=not args.quiet
        )
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    before = len(_lines(source))
    after = len(_lines(reduced))
    print(f"\nreduced {before} lines -> {after} lines in {calls} interestingness tests")
    print("=" * 60)
    print(reduced)

    if args.output:
        args.output.write_text(reduced, encoding="utf-8")
        print(f"written to {args.output}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
