# The ToyC Language Specification

Version 1.0 — frozen for the duration of this project.

ToyC is a small, deliberately C-like language. It is designed to be *just* rich
enough to make optimization worthwhile (functions, recursion, loops, arrays)
while being small enough to specify completely — which is the whole point, since
every claim this project makes about correctness is relative to this document.

---

## 1. Design principle: no undefined behaviour

**Every ToyC program has exactly one defined meaning.** Where C would say
"undefined behaviour", ToyC specifies an exact outcome.

This is not pedantry; it is what makes the project's central technique possible.
The differential tester compares three independent implementations (AST
interpreter, IR virtual machine, LLVM-compiled native code) on randomly
generated programs. If the language had undefined behaviour, a random program
could trigger it and the three implementations could *legitimately* disagree —
and every such disagreement would be a false alarm that has to be investigated
by hand. By defining everything, **any disagreement is a real bug.**

The cases that would be UB in C are listed in §7.

---

## 2. Lexical structure

### Comments

`// line comment` and `/* block comment */`. Block comments do not nest.

### Identifiers

`[A-Za-z_][A-Za-z0-9_]*`, case-sensitive.

### Keywords (reserved)

```
int  void  if  else  while  for  return  break  continue  print
```

### Integer literals

Decimal only: `[0-9]+`. A literal must fit in the range of `int` (§3);
otherwise it is a compile-time error. Note that `-2147483648` is lexed as unary
minus applied to `2147483648`, so the literal `2147483648` is permitted **only**
as the direct operand of a unary minus.

### Operators and punctuation

```
+  -  *  /  %
<  <=  >  >=  ==  !=
&&  ||  !
=
(  )  {  }  [  ]  ,  ;
```

### Whitespace

Space, tab, carriage return and newline separate tokens and are otherwise
ignored.

---

## 3. Types

ToyC has one scalar type and one aggregate type.

| Type | Meaning |
|---|---|
| `int` | A **32-bit two's-complement** signed integer. |
| `int[N]` | A one-dimensional array of `N` ints, `N >= 1`. |
| `void` | The absence of a value. Legal only as a function return type. |

There is no `bool`. Comparison and logical operators yield `int`: **`1` for true
and `0` for false**. Any nonzero `int` counts as true in a condition.

> **Why no `bool`?** It would add a second scalar type, implicit conversions,
> and a whole dimension of type-checker rules, for no optimization interest.
> C89 managed without one.

Arrays are **not** first-class values: they cannot be assigned, returned, or
compared. An array may only be indexed, or passed to a function (§4.2).

---

## 4. Program structure

```ebnf
program      ::= declaration*
declaration  ::= globalVarDecl | functionDecl
```

A program must define `int main()`. Execution begins there.

Declarations are visible from their point of declaration to the end of the
file — **except** function names, which are visible throughout the whole
program. So mutual recursion works without forward declarations.

### 4.1 Global variables

```ebnf
globalVarDecl ::= "int" IDENT [ "=" constExpr ] ";"
                | "int" IDENT "[" INT_LIT "]" ";"
```

A global's initialiser must be a constant expression (literals and the operators
of §6 applied to them — no variables, no calls). **Globals without an
initialiser, and all array elements, are initialised to `0`.**

### 4.2 Functions

```ebnf
functionDecl ::= ("int" | "void") IDENT "(" [ params ] ")" block
params       ::= param { "," param }
param        ::= "int" IDENT | "int" IDENT "[" "]"
```

Functions may not be nested. Recursion, including mutual recursion, is allowed.

An `int name[]` parameter receives an array **by reference**: writes through it
are visible to the caller. This is the one place ToyC has reference semantics,
and it exists because copying arrays would be both expensive and uninteresting.

---

## 5. Statements

```ebnf
block     ::= "{" statement* "}"
statement ::= localVarDecl
            | assignment
            | ifStmt | whileStmt | forStmt
            | "return" [ expression ] ";"
            | "break" ";" | "continue" ";"
            | expression ";"
            | block
```

### 5.1 Declarations and scope

```ebnf
localVarDecl ::= "int" IDENT [ "=" expression ] ";"
               | "int" IDENT "[" INT_LIT "]" ";"
```

Blocks introduce a scope; an inner declaration may shadow an outer one. A local
declared without an initialiser is **`0`**, and local array elements are **`0`**
(§7.4). Redeclaring a name in the *same* scope is a compile-time error.

### 5.2 Assignment

```ebnf
assignment ::= IDENT "=" expression ";"
             | IDENT "[" expression "]" "=" expression ";"
```

Assignment is a **statement, not an expression**: `a = b = c` and
`while (x = f())` do not parse. This removes a large class of confusing programs
and makes the random generator simpler.

Evaluation order is **index first, then the right-hand side**.

### 5.3 Control flow

```ebnf
ifStmt    ::= "if" "(" expression ")" statement [ "else" statement ]
whileStmt ::= "while" "(" expression ")" statement
forStmt   ::= "for" "(" [ forInit ] ";" [ expression ] ";" [ forUpdate ] ")" statement
forInit   ::= localVarDecl without the semicolon | assignment without the semicolon
forUpdate ::= assignment without the semicolon
```

A dangling `else` binds to the **nearest** unmatched `if`.

`for` is **syntactic sugar**, desugared in the parser:

```
for (init; cond; update) body   ==>   { init; while (cond) { body; update; } }
```

with two refinements: an omitted `cond` means `1`, and a `continue` inside the
body must still run `update`. A variable declared in `init` is scoped to the
loop.

`break` and `continue` are compile-time errors outside a loop.

### 5.4 Return

A `void` function uses `return;`; an `int` function uses `return expr;`.
Mixing them is a compile-time error.

**If control reaches the end of an `int` function without a `return`, the
function returns `0`.** (In C this is UB. See §1.)

### 5.5 print

`print(expr)` is a builtin, not a function: it cannot be shadowed, redefined, or
passed around. It writes the decimal value of `expr` followed by a newline
(`\n`, never `\r\n`) to standard output.

`print` is the **only** observable effect a ToyC program has, apart from its
exit status. This is deliberate — it gives the differential tester a clean,
totally-ordered trace to compare.

---

## 6. Expressions

In order of **increasing** precedence:

| Level | Operators | Associativity | Notes |
|---|---|---|---|
| 1 | `\|\|` | left | short-circuit |
| 2 | `&&` | left | short-circuit |
| 3 | `==` `!=` | left | yields 0 or 1 |
| 4 | `<` `<=` `>` `>=` | left | yields 0 or 1 |
| 5 | `+` `-` | left | |
| 6 | `*` `/` `%` | left | |
| 7 | unary `-` and `!` | right | |
| 8 | call, `[]`, literal, identifier, `( )` | — | |

### Short-circuit evaluation

`a && b` evaluates `b` **only if** `a` is nonzero; `a || b` evaluates `b` only
if `a` is zero. Both yield `0` or `1`. This is observable, because `b` may
contain a call that prints.

### Evaluation order

For every binary operator, the **left operand is fully evaluated before the
right**. Function arguments are evaluated **left to right**. (In C the latter is
unspecified. See §1.)

### Function calls

`f(a, b)` — the argument count must match the declaration. Calling a `void`
function in a context that needs a value is a compile-time error.

---

## 7. Defined behaviour where C has none

This section is the heart of the specification. Every implementation —
interpreter, VM, and LLVM backend — must agree here exactly.

### 7.1 Integer overflow — wraps

Arithmetic on `int` is **two's-complement wrapping** modulo 2^32.
`2147483647 + 1` is `-2147483648`.

> **Implementation consequence, flagged now because it affects the LLVM
> backend:** the LLVM IR emitter must emit `add`/`sub`/`mul` **without** the
> `nsw` flag, and any C translation used for cross-checking must perform
> arithmetic on `uint32_t` and convert back. Emitting `nsw` would let LLVM
> assume overflow is impossible and optimize on that assumption — producing a
> result that disagrees with the oracle. This is a genuine, classic
> miscompilation trap.

### 7.2 Division and remainder — trap

`x / 0` and `x % 0` are **runtime errors** (§7.6).

`INT_MIN / -1` would overflow; it is **also a runtime error**, not a wrap.
On x86 the hardware `idiv` faults here, so defining it as a trap costs nothing
and keeps all three backends honest.

For nonzero divisors, division **truncates toward zero** and the remainder takes
the sign of the dividend: `-7 / 2 == -3`, `-7 % 2 == -1`.

### 7.3 Array indexing — bounds-checked

Every array access is bounds-checked. An index `i` into an array of length `N`
is valid iff `0 <= i < N`; otherwise it is a **runtime error** (§7.6).

> This is the single most expensive decision in the spec — it puts a compare and
> a branch on every array access. That is intentional: it gives the optimizer
> something real to work on (redundant bounds checks are a classic elimination
> target) and it is what makes randomly generated array programs safe to run.

### 7.4 Uninitialised variables — zero

All variables, local and global, scalar and array, start at **`0`**.

### 7.5 Recursion depth — bounded

Call depth is limited to **10,000** frames. Exceeding it is a **runtime error**
(§7.6), not a crash. This keeps a randomly generated runaway recursion from
taking down the test harness.

### 7.6 Runtime errors

A runtime error prints a diagnostic to **standard error** and terminates the
program with **exit status 70**. Standard output produced before the error is
retained and must match across implementations.

The diagnostic text is *not* part of the observable behaviour that the
differential tester compares — only stdout and the exit status are.

| Condition | Message |
|---|---|
| Division by zero | `runtime error: division by zero` |
| Remainder by zero | `runtime error: remainder by zero` |
| `INT_MIN / -1` | `runtime error: integer division overflow` |
| Index out of bounds | `runtime error: array index out of bounds` |
| Call depth exceeded | `runtime error: call depth exceeded` |

### 7.7 Termination

ToyC programs may loop forever; this is legal. The test harness imposes its own
wall-clock timeout, and a timeout is treated as "no result", not as a mismatch.

---

## 8. Observable behaviour

Two executions of a ToyC program are **equivalent** iff they produce:

1. the same sequence of bytes on standard output, **and**
2. the same exit status.

The exit status is `main`'s return value reduced **modulo 256** (so `return 300`
exits with 44), or `70` if a runtime error occurred.

This two-part definition is the contract every optimization pass must preserve,
and it is exactly what the differential tester checks.

---

## 9. Not in ToyC

Deliberately excluded, with the reason:

| Feature | Why not |
|---|---|
| `bool`, `char`, `float`, pointers | Each adds type-checker surface with no optimization payoff. |
| Structs, multi-dimensional arrays | Lowering complexity without new optimization opportunities. |
| `do/while`, `switch`, `goto` | Sugar over existing control flow, or CFG complexity for its own sake. |
| Assignment as an expression | Makes random program generation and reading both harder. |
| Bitwise and shift operators | Shifts reintroduce UB (`x << 32`) that §1 works to avoid. |
| Strings, and I/O beyond `print` | Output must stay a simple, comparable trace. |
| Separate compilation, `extern` | Pure plumbing; no conceptual payoff. |

---

## 10. Example

```c
// Greatest common divisor, iteratively.
int gcd(int a, int b) {
  while (b != 0) {
    int t = b;
    b = a % b;
    a = t;
  }
  return a;
}

// Fibonacci, recursively -- exercises the call path.
int fib(int n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

int sieveLimit = 20;

int main() {
  print(gcd(48, 18));        // 6
  print(fib(10));            // 55

  int primes[20];
  int i = 2;
  while (i < sieveLimit) {
    if (primes[i] == 0) {
      print(i);              // 2 3 5 7 11 13 17 19
      int j = i + i;
      for (; j < sieveLimit; j = j + i) {
        primes[j] = 1;
      }
    }
    i = i + 1;
  }
  return 0;
}
```
