#!/usr/bin/env bash
# NSTU local verification gate.
#
# Antigravity has no hooks, so project rules cannot be enforced mechanically at
# edit time. This script is the enforcement point: run it before reporting any
# work as done. It is NECESSARY BUT NOT SUFFICIENT -- the real gate is MSVC
# /W4 /WX Release in GitHub Actions. Use --strict to mirror CI's warning
# posture locally.
#
#   ./scripts/verify-gate.sh            fast: reuse build/mingw, zero-warning + ctest
#   ./scripts/verify-gate.sh --strict   clean Release build with -Werror in build/gate

set -u

STRICT=0
[ "${1:-}" = "--strict" ] && STRICT=1

command -v git >/dev/null 2>&1 || {
    echo "FAIL: git is not on PATH. Run this from Git Bash"
    echo "      (C:\\Program Files\\Git\\bin\\bash.exe), not MSYS bash."
    exit 2; }
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "FAIL: not inside a git repository"; exit 2; }
cd "$ROOT" || exit 2

# UCRT64 must precede /mingw64/bin or the test exes die with 0xc0000139.
export PATH="/c/msys64/ucrt64/bin:$PATH"

FAILED=0
step()  { printf '\n=== %s ===\n' "$1"; }
pass()  { printf '  PASS  %s\n' "$1"; }
fail()  { printf '  FAIL  %s\n' "$1"; FAILED=1; }
note()  { printf '  NOTE  %s\n' "$1"; }

# ---------------------------------------------------------------- rule checks
step "Rule: no banned vendor names in code, targets, or filenames"
BANNED='moonlight|sunshine'

hits_path="$(git ls-files | grep -iE "$BANNED" || true)"
if [ -n "$hits_path" ]; then
    fail "tracked file paths contain a banned name:"
    printf '        %s\n' $hits_path
else
    pass "no banned filenames"
fi

hits_cmake="$(git ls-files '*CMakeLists.txt' '*.cmake' \
    | xargs grep -inE "(add_library|add_executable|target_link_libraries|add_test).*($BANNED)" 2>/dev/null || true)"
if [ -n "$hits_cmake" ]; then
    fail "CMake target names contain a banned name:"
    printf '        %s\n' "$hits_cmake"
else
    pass "no banned CMake target names"
fi

# Vendor names are allowed inside UI strings/comments. Flag only occurrences on
# lines with no double quote -- crude, but it catches identifiers.
hits_ident="$(git ls-files '*.cpp' '*.hpp' '*.h' \
    | xargs grep -inE "$BANNED" 2>/dev/null | grep -v '"' || true)"
if [ -n "$hits_ident" ]; then
    fail "possible banned identifier outside a string literal (review):"
    printf '        %s\n' "$hits_ident"
else
    pass "no banned identifiers outside string literals"
fi

step "Rule: forbidden paths are not staged"
staged="$(git diff --cached --name-only 2>/dev/null || true)"
bad_staged="$(printf '%s\n' "$staged" | grep -E '^(artifacts/|\.claude/|HANDOFF\.md)' || true)"
if [ -n "$bad_staged" ]; then
    fail "these must never be staged:"
    printf '        %s\n' $bad_staged
else
    pass "artifacts/, .claude/, HANDOFF.md not staged"
fi

step "Rule: no weakened tests in the working tree"
weak="$(git diff -U0 -- tests/ 2>/dev/null \
    | grep -E '^\-.*(assert|EXPECT_|REQUIRE)' || true)"
if [ -n "$weak" ]; then
    note "assertions were removed from tests/ -- confirm this is intentional:"
    printf '        %s\n' "$weak" | head -20
else
    pass "no assertions removed from tests/"
fi

# ---------------------------------------------------------------- build + test
BUILD_OK=1
if [ "$STRICT" -eq 1 ]; then
    BUILD_DIR="build/gate"
    step "Build (strict: Release + NSTU_ENABLE_WERROR=ON, mirrors CI warnings)"
    # -Wno-free-nonheap-object: GCC 15 raises a FALSE POSITIVE inside libstdc++'s
    # new_allocator.h at -O2 (pointer '<unknown>', inlined from a plain
    # std::vector insert path in common/src/discovery.cpp). The code performs no
    # manual deallocation. MSVC /WX -- the authoritative gate -- is clean on it.
    # Suppressed deliberately so a spurious diagnostic cannot pressure anyone
    # into changing correct code.
    if ! cmake -S . -B "$BUILD_DIR" -G "MinGW Makefiles"               -DCMAKE_BUILD_TYPE=Release -DNSTU_ENABLE_WERROR=ON               -DCMAKE_CXX_FLAGS="-Wno-free-nonheap-object" >/tmp/nstu-cfg.log 2>&1; then
        fail "configure failed:"
        tail -20 /tmp/nstu-cfg.log | sed 's/^/        /'
        BUILD_OK=0
    fi
else
    BUILD_DIR="build/mingw"
    step "Build (fast: reuse $BUILD_DIR; warnings are failures here)"
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        fail "$BUILD_DIR is not configured"
        BUILD_OK=0
    fi
fi

BUILD_LOG="$(mktemp)"
if [ "$BUILD_OK" -eq 1 ]; then
    if cmake --build "$BUILD_DIR" -j 8 >"$BUILD_LOG" 2>&1; then
        pass "build succeeded"
    else
        fail "build failed:"
        grep -nE "error:|Error [0-9]+" "$BUILD_LOG" | head -12 | sed 's/^/        /'
        BUILD_OK=0
    fi
fi

# Under -Werror GCC emits these as "error:", not "warning:", so count both and
# also catch the conversion notice. Never report warning-cleanliness for a build
# that did not complete -- that was a real bug in the first version of this gate.
if [ "$BUILD_OK" -eq 1 ]; then
    warn_count="$(grep -ciE 'warning:|warning C[0-9]+' "$BUILD_LOG" || true)"
    if [ "${warn_count:-0}" -gt 0 ]; then
        fail "$warn_count compiler warning(s) -- CI builds with /WX and will reject these:"
        grep -iE 'warning:|warning C[0-9]+' "$BUILD_LOG" | head -15 | sed 's/^/        /'
    else
        pass "zero compiler warnings"
    fi
elif grep -qiE 'warnings being treated as errors' "$BUILD_LOG"; then
    fail "build was failed BY a warning (-Werror):"
    grep -iE "\[-Werror=[a-z=-]+\]" "$BUILD_LOG" | head -5 | sed 's/^/        /'
else
    note "warning check skipped -- build did not complete"
fi

step "Tests"
if [ "$BUILD_OK" -eq 0 ]; then
    note "skipped -- build did not complete (running them would report 44 bogus failures)"
else
    TEST_LOG="$(mktemp)"
    if ctest --test-dir "$BUILD_DIR" --output-on-failure -j4 >"$TEST_LOG" 2>&1; then
        pass "$(grep -E 'tests passed' "$TEST_LOG" | tail -1)"
    else
        fail "ctest failed:"
        grep -E 'Failed|tests passed' "$TEST_LOG" | head -15 | sed 's/^/        /'
    fi
    rm -f "$TEST_LOG"
fi
rm -f "$BUILD_LOG"

# ---------------------------------------------------------------- verdict
printf '\n========================================\n'
if [ "$FAILED" -eq 0 ]; then
    printf 'LOCAL GATE: PASS\n'
    [ "$STRICT" -eq 0 ] && printf 'Run --strict before declaring done (CI is MSVC /WX Release).\n'
    printf 'This does NOT prove CI will pass: CI uses MSVC, not GCC.\n'
    exit 0
fi
printf 'LOCAL GATE: FAIL -- do not report this work as done.\n'
exit 1
