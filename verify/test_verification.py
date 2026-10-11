"""Pytest suite for the verification layer.

This is what CI runs. It covers three things:

  1. The generator produces programs the compiler accepts (a generator that
     mostly emits garbage would make a fuzzing campaign look clean while
     testing almost nothing).
  2. All execution paths agree on a set of hand-written programs whose expected
     output is written out by hand -- so a bug that affected every path
     identically would still be caught.
  3. A short randomised campaign, as a smoke test. The long campaigns run
     separately; this one has to finish in CI time.

Run with:   pytest verify/ -v
"""

from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from verify.differential import check_program
from verify.generator import generate_program
from verify.toolchain import Toolchain, ToolchainError

PIPELINE = ["loadelim", "copyprop", "constfold", "cse", "copyprop", "dce"]


@pytest.fixture(scope="session")
def toolchain() -> Toolchain:
    try:
        return Toolchain.discover()
    except ToolchainError as exc:
        pytest.skip(str(exc))


@pytest.fixture(scope="session")
def native_available(toolchain: Toolchain) -> bool:
    return toolchain.has_native


def write(tmp_path: pathlib.Path, source: str) -> pathlib.Path:
    path = tmp_path / "program.tc"
    path.write_text(source, encoding="utf-8")
    return path


# =============================================================================
# Hand-written programs with hand-written expected output
# =============================================================================
#
# Every other test in this project compares implementations against each other.
# These compare against a human. A bug that affected all three paths the same
# way -- a misreading of the spec, say -- would pass every differential test and
# only be caught here.

GOLDEN = [
    ("arithmetic", "int main() { print(2 + 3 * 4); return 0; }", "14\n", 0),
    ("truncating_div", "int main() { print(-7 / 2); print(-7 % 2); return 0; }", "-3\n-1\n", 0),
    ("overflow_wraps", "int main() { print(2147483647 + 1); return 0; }", "-2147483648\n", 0),
    (
        "short_circuit",
        "int f() { print(99); return 1; } int main() { if (0 && f()) { print(1); } print(2); return 0; }",
        "2\n",
        0,
    ),
    (
        "fib",
        "int fib(int n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } "
        "int main() { print(fib(10)); return 0; }",
        "55\n",
        0,
    ),
    (
        "array_by_reference",
        "void fill(int a[], int n) { for (int i = 0; i < n; i = i + 1) { a[i] = i * i; } } "
        "int main() { int d[4]; fill(d, 4); "
        "for (int i = 0; i < 4; i = i + 1) { print(d[i]); } return 0; }",
        "0\n1\n4\n9\n",
        0,
    ),
    ("uninitialised_is_zero", "int main() { int x; int a[2]; print(x); print(a[1]); return 0; }", "0\n0\n", 0),
    ("exit_status_mod_256", "int main() { print(1); return 300; }", "1\n", 44),
    ("negative_exit_status", "int main() { return -1; }", "", 255),
    # The spec's trapping cases. Output produced before the trap must survive.
    ("trap_div_zero", "int main() { print(1); int z = 0; print(9 / z); return 0; }", "1\n", 70),
    ("trap_bounds", "int main() { print(1); int a[2]; print(a[5]); return 0; }", "1\n", 70),
    (
        "trap_int_min_div",
        "int main() { print(1); int a = -2147483648; int b = -1; print(a / b); return 0; }",
        "1\n",
        70,
    ),
    (
        "missing_return_is_zero",
        "int f(int n) { if (n > 100) { return 1; } } int main() { print(f(1)); return 0; }",
        "0\n",
        0,
    ),
]


@pytest.mark.parametrize("name,source,expected_out,expected_status", GOLDEN, ids=[g[0] for g in GOLDEN])
def test_oracle_matches_hand_written_expectation(
    toolchain: Toolchain, tmp_path: pathlib.Path, name: str, source: str, expected_out: str, expected_status: int
):
    path = write(tmp_path, source)
    result = toolchain.run_oracle(path)
    assert result.stdout == expected_out
    assert result.exit_status == expected_status


@pytest.mark.parametrize("name,source,expected_out,expected_status", GOLDEN, ids=[g[0] for g in GOLDEN])
def test_vm_matches_hand_written_expectation(
    toolchain: Toolchain, tmp_path: pathlib.Path, name: str, source: str, expected_out: str, expected_status: int
):
    path = write(tmp_path, source)
    result = toolchain.run_vm(path, PIPELINE)
    assert result.stdout == expected_out
    assert result.exit_status == expected_status


@pytest.mark.parametrize("name,source,expected_out,expected_status", GOLDEN, ids=[g[0] for g in GOLDEN])
def test_native_matches_hand_written_expectation(
    toolchain: Toolchain,
    native_available: bool,
    tmp_path: pathlib.Path,
    name: str,
    source: str,
    expected_out: str,
    expected_status: int,
):
    if not native_available:
        pytest.skip("clang not available")
    path = write(tmp_path, source)
    result = toolchain.run_native(path, PIPELINE)
    assert result.stdout == expected_out
    # The exit-status case the fuzzer caught: on Windows the OS does NOT
    # truncate to 8 bits, so the emitted code has to do it.
    assert result.exit_status == expected_status


# =============================================================================
# The generator
# =============================================================================


@pytest.mark.parametrize("seed", range(20))
def test_generated_programs_compile(toolchain: Toolchain, tmp_path: pathlib.Path, seed: int):
    path = write(tmp_path, generate_program(seed, size="small"))
    assert toolchain.compiles(path), "the generator produced a program the front end rejects"


def test_generator_is_deterministic():
    # A fuzzer finding is only useful if its seed reproduces it exactly.
    assert generate_program(123) == generate_program(123)
    assert generate_program(123) != generate_program(124)


def test_generator_produces_varied_programs():
    programs = [generate_program(s, size="medium") for s in range(30)]
    assert len(set(programs)) == len(programs)

    combined = "\n".join(programs)
    for construct in ["for (", "while (", "if (", "print(", "[", "&&", "||"]:
        assert construct in combined, f"the generator never emits {construct!r}"


# =============================================================================
# Differential smoke tests
# =============================================================================


@pytest.mark.parametrize("seed", range(15))
def test_random_program_paths_agree(toolchain: Toolchain, seed: int):
    outcome = check_program(
        toolchain, generate_program(seed, size="small"), seed=seed, include_native=toolchain.has_native
    )
    assert not outcome.mismatches, "\n\n".join(m.describe() for m in outcome.mismatches)


@pytest.mark.parametrize(
    "passes",
    [
        [],
        ["loadelim"],
        ["constfold"],
        ["dce"],
        ["cse"],
        ["copyprop"],
        PIPELINE,
        # Deliberately silly orders: correctness must not depend on the order
        # being sensible, because the ML scheduler is free to pick any of them.
        ["dce", "cse", "constfold", "copyprop", "loadelim"],
        ["dce", "dce", "dce"],
    ],
    ids=["none", "loadelim", "constfold", "dce", "cse", "copyprop", "O2", "reversed", "dce-thrice"],
)
def test_pipelines_preserve_behaviour(toolchain: Toolchain, tmp_path: pathlib.Path, passes: list[str]):
    for seed in range(8):
        source = generate_program(seed, size="small")
        path = write(tmp_path, source)
        if not toolchain.compiles(path):
            continue

        oracle = toolchain.run_oracle(path)
        if oracle.timed_out:
            continue

        optimized = toolchain.run_vm(path, passes or None)
        assert oracle.observably_equals(optimized), (
            f"pipeline {passes} changed behaviour on seed {seed}\n{source}"
        )


def test_optimization_never_increases_dynamic_instructions(toolchain: Toolchain, tmp_path: pathlib.Path):
    """-O2 must never make a program do more work than -O0.

    Not a correctness property, but a regression guard: a pass that made
    programs slower would still pass every differential test while quietly
    undermining the point of the optimizer.
    """
    checked = 0
    for seed in range(12):
        path = write(tmp_path, generate_program(seed, size="small"))
        if not toolchain.compiles(path):
            continue

        baseline = toolchain.measure(path, None)
        optimized = toolchain.measure(path, PIPELINE)
        if baseline is None or optimized is None:
            continue

        assert optimized["dynamic_instructions"] <= baseline["dynamic_instructions"], (
            f"-O2 made seed {seed} slower"
        )
        checked += 1

    assert checked > 0, "no programs were actually measured"


def test_features_are_extractable(toolchain: Toolchain, tmp_path: pathlib.Path):
    path = write(tmp_path, generate_program(5, size="medium"))
    features = toolchain.features(path)

    assert features is not None
    assert len(features) >= 20
    for name, value in features.items():
        # A NaN would silently poison any model trained on it.
        assert value == value, f"feature {name} is NaN"
