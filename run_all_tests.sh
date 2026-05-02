#!/bin/bash
# ============================================================================
# run_all_tests.sh — Automated test runner for LLVM MPI Sanitizer
#
# Builds the project (if needed), compiles each test through the LLVM pass
# pipeline, runs with mpirun, and checks stderr output for expected
# sanitizer diagnostics.
#
# Exit code: 0 if all tests pass, 1 if any fail.
# ============================================================================

set -euo pipefail

# ─── Paths & flags ──────────────────────────────────────────────────────────
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
TEST_DIR="${PROJECT_DIR}/tests"
EXAMPLE_DIR="${PROJECT_DIR}/examples"
TMP_DIR="${BUILD_DIR}/test_tmp"
EVAL_DIR="${PROJECT_DIR}/evaluation"

PASS_PLUGIN="${BUILD_DIR}/libMPISanitizePass.so"
RUNTIME_LIB="${BUILD_DIR}/libmsan_runtime.so"

MPI_CFLAGS=$(mpicc --showme:compile 2>/dev/null || echo "")
MPI_LDFLAGS=$(mpicc --showme:link 2>/dev/null || echo "")

PASS_COUNT=0
FAIL_COUNT=0
TOTAL_COUNT=0

# ─── Colours ────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# ─── Build if needed ────────────────────────────────────────────────────────
build_project() {
    if [ ! -f "${PASS_PLUGIN}" ] || [ ! -f "${RUNTIME_LIB}" ]; then
        echo -e "${CYAN}[BUILD]${NC} Building project..."
        mkdir -p "${BUILD_DIR}"
        cd "${BUILD_DIR}"
        cmake "${PROJECT_DIR}" -DCMAKE_BUILD_TYPE=Debug
        make -j"$(nproc)"
        cd "${PROJECT_DIR}"
        echo -e "${GREEN}[BUILD]${NC} Build complete."
    else
        echo -e "${GREEN}[BUILD]${NC} Build artifacts found, skipping build."
    fi
}

# ─── Compile & run pipeline ─────────────────────────────────────────────────
# compile_and_run <source_file> <num_ranks>
#
# 1. Compile source to .bc with clang -g -O0 -emit-llvm
# 2. Run opt with the pass plugin (mpi-sanitize)
# 3. Compile instrumented .bc to object, link with msan_runtime + MPI
# 4. Run with mpirun --oversubscribe -np <num_ranks>
# 5. Captures stderr output and returns it via global STDERR_OUTPUT
# Returns exit code of the mpirun invocation.
STDERR_OUTPUT=""

compile_and_run() {
    local src="$1"
    local np="$2"
    local base
    base="$(basename "${src}" .c)"

    mkdir -p "${TMP_DIR}"

    local bc_file="${TMP_DIR}/${base}.bc"
    local inst_bc="${TMP_DIR}/${base}.inst.bc"
    local obj_file="${TMP_DIR}/${base}.o"
    local exe_file="${TMP_DIR}/${base}"
    local stderr_file="${TMP_DIR}/${base}.stderr"

    # Step 1: Compile to LLVM bitcode
    clang -g -O0 -emit-llvm -c ${MPI_CFLAGS} "${src}" -o "${bc_file}" 2>/dev/null
    if [ $? -ne 0 ]; then
        STDERR_OUTPUT="COMPILE_ERROR"
        return 1
    fi

    # Step 2: Run the sanitizer pass
    opt -load-pass-plugin="${PASS_PLUGIN}" -passes="mpi-sanitize" \
        "${bc_file}" -o "${inst_bc}" 2>/dev/null
    if [ $? -ne 0 ]; then
        STDERR_OUTPUT="OPT_ERROR"
        return 1
    fi

    # Step 3: Compile instrumented bitcode to object and link
    clang -g -O0 -c "${inst_bc}" -o "${obj_file}" 2>/dev/null
    if [ $? -ne 0 ]; then
        STDERR_OUTPUT="LINK_COMPILE_ERROR"
        return 1
    fi

    # Link with MPI and the runtime library
    mpicc -g -o "${exe_file}" "${obj_file}" \
        -L"${BUILD_DIR}" -lmsan_runtime -lm \
        -Wl,-rpath,"${BUILD_DIR}" 2>/dev/null
    if [ $? -ne 0 ]; then
        STDERR_OUTPUT="LINK_ERROR"
        return 1
    fi

    # Step 4: Run with mpirun
    local run_exit=0
    timeout 60 mpirun --oversubscribe -np "${np}" "${exe_file}" \
        2>"${stderr_file}" 1>/dev/null || run_exit=$?

    # Step 5: Capture stderr
    STDERR_OUTPUT="$(cat "${stderr_file}" 2>/dev/null || echo "")"

    return 0
}

# ─── Test runner ─────────────────────────────────────────────────────────────
# run_test <name> <source_file> <num_ranks> <expected_tag> <description>
#
# expected_tag: empty string "" for correct programs, or the [msan][...] tag
#               substring to look for in stderr. Multiple expected tags can
#               be separated by "|" (any one match = pass).
run_test() {
    local name="$1"
    local src="$2"
    local np="$3"
    local expected="$4"
    local desc="$5"

    TOTAL_COUNT=$((TOTAL_COUNT + 1))

    if [ ! -f "${src}" ]; then
        echo -e "  ${RED}[FAIL]${NC} ${BOLD}${name}${NC} — source file not found: ${src}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi

    compile_and_run "${src}" "${np}"
    local rc=$?

    if [ "${STDERR_OUTPUT}" = "COMPILE_ERROR" ]; then
        echo -e "  ${RED}[FAIL]${NC} ${BOLD}${name}${NC} — compilation error"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi
    if [ "${STDERR_OUTPUT}" = "OPT_ERROR" ]; then
        echo -e "  ${RED}[FAIL]${NC} ${BOLD}${name}${NC} — opt pass error"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi
    if [ "${STDERR_OUTPUT}" = "LINK_COMPILE_ERROR" ] || [ "${STDERR_OUTPUT}" = "LINK_ERROR" ]; then
        echo -e "  ${RED}[FAIL]${NC} ${BOLD}${name}${NC} — link error"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        return
    fi

    # Check for expected msan output
    if [ -z "${expected}" ]; then
        # Correct program — should have NO [msan] error tags (info is OK)
        if echo "${STDERR_OUTPUT}" | grep -qE '\[msan\]\[(type-mismatch|size-mismatch|unmatched-send|unmatched-recv|deadlock-detected|collective-mismatch|overlap-warning|anomaly-warning|integrity-violation)\]'; then
            echo -e "  ${RED}[FAIL]${NC} ${BOLD}${name}${NC} — ${desc}"
            echo -e "         expected no errors, but sanitizer reported errors"
            FAIL_COUNT=$((FAIL_COUNT + 1))
        else
            echo -e "  ${GREEN}[PASS]${NC} ${BOLD}${name}${NC} — ${desc}"
            PASS_COUNT=$((PASS_COUNT + 1))
        fi
    else
        # Buggy program — should have the expected tag(s)
        local found=0
        IFS='|' read -ra TAGS <<< "${expected}"
        local missing=""
        for tag in "${TAGS[@]}"; do
            if echo "${STDERR_OUTPUT}" | grep -q "${tag}"; then
                found=$((found + 1))
            else
                if [ -n "${missing}" ]; then
                    missing="${missing}, ${tag}"
                else
                    missing="${tag}"
                fi
            fi
        done

        if [ ${found} -eq ${#TAGS[@]} ]; then
            echo -e "  ${GREEN}[PASS]${NC} ${BOLD}${name}${NC} — ${desc}"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            echo -e "  ${RED}[FAIL]${NC} ${BOLD}${name}${NC} — ${desc}"
            echo -e "         missing expected tag(s): ${missing}"
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    fi
}

# ═══════════════════════════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════════════════════════

echo ""
echo -e "${BOLD}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  LLVM MPI Sanitizer — Test Suite                            ${NC}"
echo -e "${BOLD}═══════════════════════════════════════════════════════════════${NC}"
echo ""

build_project

echo ""
echo -e "${BOLD}─── Running Tests ─────────────────────────────────────────────${NC}"
echo ""

# ─── Existing example tests ─────────────────────────────────────────────────
echo -e "${YELLOW}  Examples (existing):${NC}"
run_test "type_mismatch"        "${EXAMPLE_DIR}/type_mismatch.c"        2 "type-mismatch"       "detects MPI_INT vs MPI_DOUBLE type mismatch"
run_test "size_mismatch"        "${EXAMPLE_DIR}/size_mismatch.c"        2 "size-mismatch"       "detects send count=4 vs recv count=2"
run_test "collective_mismatch"  "${EXAMPLE_DIR}/collective_mismatch.c"  2 "collective-mismatch" "detects Bcast root disagreement"
echo ""

# ─── Correct programs (should produce NO errors) ────────────────────────────
echo -e "${YELLOW}  Correct programs (no errors expected):${NC}"
run_test "correct_pingpong"    "${TEST_DIR}/correct_pingpong.c"    2 "" "no errors detected (correct ping-pong)"
run_test "correct_broadcast"   "${TEST_DIR}/correct_broadcast.c"   4 "" "no errors detected (correct broadcast)"
run_test "correct_reduce"      "${TEST_DIR}/correct_reduce.c"      4 "" "no errors detected (correct reduce)"
run_test "correct_multi_send"  "${TEST_DIR}/correct_multi_send.c"  4 "" "no errors detected (correct multi-send)"
run_test "correct_allreduce"   "${TEST_DIR}/correct_allreduce.c"   4 "" "no errors detected (correct allreduce)"
echo ""

# ─── Buggy programs (sanitizer MUST detect errors) ──────────────────────────
echo -e "${YELLOW}  Buggy programs (errors expected):${NC}"
run_test "deadlock"                "${TEST_DIR}/deadlock.c"                2 "unmatched-send|deadlock-detected"      "detects unmatched sends and deadlock cycle"
run_test "wrong_tag"               "${TEST_DIR}/wrong_tag.c"               2 "unmatched-send"                        "detects tag mismatch (unmatched send)"
run_test "wrong_tag_nonblocking"   "${TEST_DIR}/wrong_tag_nonblocking.c"   2 "type-mismatch"                         "detects type mismatch in cross-send"
run_test "reduce_wrong_root"       "${TEST_DIR}/reduce_wrong_root.c"       2 "collective-mismatch"                   "detects MPI_Reduce root disagreement"
run_test "bcast_wrong_root"        "${TEST_DIR}/bcast_wrong_root.c"        2 "collective-mismatch"                   "detects MPI_Bcast root disagreement"
run_test "large_message_anomaly"   "${TEST_DIR}/large_message_anomaly.c"   2 "anomaly-warning"                       "detects anomalous message size"
run_test "multi_type_errors"       "${TEST_DIR}/multi_type_errors.c"       2 "type-mismatch|size-mismatch"           "detects both type and size mismatch"
echo ""

# ─── Evaluation (real MPI application) ───────────────────────────────────────
echo -e "${YELLOW}  Evaluation (real MPI application):${NC}"
run_test "miniapp_eval"            "${EVAL_DIR}/miniapp_eval.c"            4 ""                                      "Jacobi iteration mini-app (real MPI evaluation)"
echo ""

# ─── Results ─────────────────────────────────────────────────────────────────
echo -e "${BOLD}─── Results ───────────────────────────────────────────────────${NC}"
echo ""
echo -e "  Total:  ${TOTAL_COUNT}"
echo -e "  ${GREEN}Passed: ${PASS_COUNT}${NC}"
echo -e "  ${RED}Failed: ${FAIL_COUNT}${NC}"
echo ""

if [ ${FAIL_COUNT} -eq 0 ]; then
    echo -e "${GREEN}${BOLD}  ✓ All tests passed!${NC}"
    echo ""
    exit 0
else
    echo -e "${RED}${BOLD}  ✗ ${FAIL_COUNT} test(s) failed.${NC}"
    echo ""
    exit 1
fi
