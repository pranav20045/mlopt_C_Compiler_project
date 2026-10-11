/*
 * The ToyC runtime, compiled alongside the LLVM IR that mloptc emits.
 *
 * Writing these helpers in C rather than hand-writing them as LLVM IR text
 * keeps the emitter small and keeps the tricky parts -- bounds checks, trap
 * semantics, array lifetime -- in a form that can actually be read and
 * reviewed. clang compiles this and the generated .ll together.
 *
 * Everything here exists to make the native backend agree EXACTLY with the AST
 * interpreter and the IR VM, including where programs trap. See docs/spec.md
 * section 7.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Must match mloptc::kMaxCallDepth in include/mloptc/execution.hpp. */
#define MLOPTC_MAX_DEPTH 1000

/*
 * Maximum number of arrays live at one time. The interpreter and VM grow their
 * arena without limit; this backend uses a fixed table, so a program that keeps
 * more than this many arrays alive at once is rejected rather than silently
 * diverging. Documented as a backend limitation in docs/DESIGN.md.
 */
#define MLOPTC_MAX_ARRAYS 65536

static int32_t* g_arena_ptr[MLOPTC_MAX_ARRAYS];
static int32_t g_arena_len[MLOPTC_MAX_ARRAYS];
static int32_t g_arena_count = 0;
static int32_t g_depth = 0;

/*
 * Every runtime error exits with status 70 (spec 7.6).
 *
 * No diagnostic is printed, and that is deliberate: per the spec only stdout
 * and the exit status are observable, and writing to stderr from generated code
 * would mean depending on `stderr` being addressable, which varies by platform.
 * The exit status is what the differential tester compares.
 *
 * exit() flushes stdout, so output produced before the trap is retained -- the
 * spec requires that, and it is what makes "print 1, then trap" comparable
 * across all three backends.
 */
static void mloptc_trap(void) {
  fflush(stdout);
  exit(70);
}

void mloptc_print(int32_t value) {
  /* "%d\n", never "%d\r\n": the byte sequence must match the other backends. */
  printf("%d\n", value);
}

/*
 * Division and remainder, with the two cases the spec defines as traps.
 *
 * These MUST be function calls rather than a bare LLVM `sdiv`: `sdiv` is
 * undefined on a zero divisor and on INT_MIN / -1, so emitting it directly
 * would turn a specified trap into undefined behaviour, and clang -O2 would be
 * free to assume it never happens.
 */
int32_t mloptc_div(int32_t a, int32_t b) {
  if (b == 0) {
    mloptc_trap();
  }
  if (a == INT32_MIN && b == -1) {
    mloptc_trap();
  }
  return a / b;
}

int32_t mloptc_rem(int32_t a, int32_t b) {
  if (b == 0) {
    mloptc_trap();
  }
  if (a == INT32_MIN && b == -1) {
    mloptc_trap();
  }
  return a % b;
}

/*
 * Arrays are referred to by an integer HANDLE, not a pointer, exactly as in the
 * IR VM. That is what lets a handle live in an ordinary i32 frame slot and be
 * passed as an ordinary i32 argument, which in turn is what gives array
 * parameters reference semantics (spec 4.2) without the IR needing a pointer
 * type at all.
 */
int32_t mloptc_array_new(int32_t n) {
  if (n < 1 || g_arena_count >= MLOPTC_MAX_ARRAYS) {
    mloptc_trap();
  }
  /* calloc, so elements start at zero (spec 7.4). */
  int32_t* data = (int32_t*)calloc((size_t)n, sizeof(int32_t));
  if (data == NULL) {
    mloptc_trap();
  }
  const int32_t handle = g_arena_count++;
  g_arena_ptr[handle] = data;
  g_arena_len[handle] = n;
  return handle;
}

/* Bounds-checked address of one element (spec 7.3). */
int32_t* mloptc_array_addr(int32_t handle, int32_t index) {
  if (handle < 0 || handle >= g_arena_count) {
    mloptc_trap();
  }
  if (index < 0 || index >= g_arena_len[handle]) {
    mloptc_trap();
  }
  return g_arena_ptr[handle] + index;
}

/*
 * Called on entry to every ToyC function. Enforces the call-depth limit (spec
 * 7.5) and returns a mark that mloptc_leave uses to free the arrays this call
 * allocated -- mirroring the arena truncation the interpreter and VM do, so
 * array lifetimes are identical in all three.
 */
int32_t mloptc_enter(void) {
  if (++g_depth > MLOPTC_MAX_DEPTH) {
    mloptc_trap();
  }
  return g_arena_count;
}

void mloptc_leave(int32_t mark) {
  for (int32_t i = mark; i < g_arena_count; ++i) {
    free(g_arena_ptr[i]);
    g_arena_ptr[i] = NULL;
    g_arena_len[i] = 0;
  }
  g_arena_count = mark;
  --g_depth;
}
