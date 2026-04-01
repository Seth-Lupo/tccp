#!/bin/bash
# Unified test runner for tccp.
#
# Usage: ./scripts/test.sh [suite] [mode] [filter]
#
# Suites:
#   unit          Unit tests only (default, no cluster needed)
#   integration   Integration tests only (needs cluster)
#   all           Both unit and integration
#
# Modes:
#   (none)        Run all tests in the suite (default)
#   failed        Re-run only tests that failed last run
#   new           Run only tests added since the last snapshot
#   new+failed    Run both new and failed tests
#
# Filter:
#   Optional gtest_filter pattern, e.g. "StateStore*" or "*Sync*"
#
# Examples:
#   ./scripts/test.sh                          # all unit tests
#   ./scripts/test.sh unit                     # all unit tests (explicit)
#   ./scripts/test.sh integration              # all integration tests
#   ./scripts/test.sh all                      # unit + integration
#   ./scripts/test.sh unit failed              # re-run failed unit tests
#   ./scripts/test.sh integration new          # new integration tests only
#   ./scripts/test.sh unit new+failed          # new + failed unit tests
#   ./scripts/test.sh unit "" "StateStore*"    # unit tests matching filter
set -e
cd "$(dirname "$0")/.."

SUITE="${1:-unit}"
MODE="${2:-}"
FILTER="${3:-}"

mkdir -p test-results

# ── Helpers ──────────────────────────────────────────────────────

parse_results() {
    local xml="$1" label="$2"
    local failures="${xml%.xml}-failures.txt"
    local known="${xml%.xml}-known.txt"

    if [ ! -f "$xml" ]; then return 0; fi

    python3 -c "
import xml.etree.ElementTree as ET
tree = ET.parse('$xml')
all_tests, failed = [], []
for tc in tree.iter('testcase'):
    name = tc.get('classname') + '.' + tc.get('name')
    all_tests.append(name)
    if tc.findall('failure'):
        failed.append(name)
with open('$failures', 'w') as f:
    for t in failed: f.write(t + '\n')
with open('$known', 'w') as f:
    for t in sorted(all_tests): f.write(t + '\n')
" 2>/dev/null

    local count
    count=$(wc -l < "$failures" | tr -d ' ')
    if [ "$count" -gt 0 ]; then
        echo "$label: $count failed test(s) saved to $failures"
    else
        rm -f "$failures"
    fi
}

build_filter_from_file() {
    local file="$1"
    if [ -f "$file" ]; then
        paste -sd ':' "$file"
    fi
    return 0
}

detect_new_tests() {
    local binary="$1" known="$2"
    if [ ! -f "$known" ]; then
        echo "No snapshot at $known — run the full suite first to establish a baseline." >&2
        return 0
    fi
    local current
    current=$("$binary" --gtest_list_tests 2>/dev/null | python3 -c "
import sys
suite, tests = '', []
for line in sys.stdin:
    line = line.rstrip()
    if not line: continue
    if line.endswith('.'): suite = line
    elif line.startswith('  '): tests.append(suite + line.strip())
for t in sorted(tests): print(t)
")
    local new_tests
    new_tests=$(comm -23 <(echo "$current") <(sort "$known"))
    if [ -n "$new_tests" ]; then
        echo "$new_tests" | paste -sd ':'
    fi
}

build_mode_filter() {
    local mode="$1" binary="$2" failures="$3" known="$4"
    local parts=()

    if [[ "$mode" == *"failed"* ]]; then
        local f
        f=$(build_filter_from_file "$failures")
        [ -n "$f" ] && parts+=("$f")
    fi
    if [[ "$mode" == *"new"* ]]; then
        local n
        n=$(detect_new_tests "$binary" "$known")
        [ -n "$n" ] && parts+=("$n")
    fi

    if [ ${#parts[@]} -eq 0 ]; then
        return 0
    fi
    IFS=':'; echo "${parts[*]}"
}

# Run a single suite. Returns the gtest exit code.
run_one() {
    local label="$1" target="$2" binary="$3" xml="$4" log="$5" filter="$6"

    cmake --build build --target "$target" -j4 2>&1 | tail -1

    local args=("--gtest_output=xml:$xml")
    [ -n "$filter" ] && args+=("--gtest_filter=$filter")

    echo "=== $label ==="
    set +e
    "$binary" "${args[@]}" 2>&1 | tee "$log"
    local rc=${PIPESTATUS[0]}
    set -e

    parse_results "$xml" "$label"
    return $rc
}

# ── Mode filter resolution ───────────────────────────────────────

resolve_filter() {
    local mode="$1" filter="$2" binary="$3" suite_name="$4"
    local failures="test-results/${suite_name}-failures.txt"
    local known="test-results/${suite_name}-known.txt"

    if [ -n "$filter" ]; then
        echo "$filter"
    elif [ -n "$mode" ]; then
        local mf
        mf=$(build_mode_filter "$mode" "$binary" "$failures" "$known")
        if [ -z "$mf" ]; then
            echo "No ${mode} ${suite_name} tests to run." >&2
            return 1
        fi
        echo "$mf"
    fi
    return 0
}

# ── Suite runners ────────────────────────────────────────────────

run_unit() {
    local f
    f=$(resolve_filter "$MODE" "$FILTER" build/tests/tccp_tests unit) || return 0
    run_one "Unit Tests" tccp_tests build/tests/tccp_tests \
        test-results/unit.xml test-results/unit.log "$f"
}

run_integration() {
    export TCCP_INTEGRATION=1
    local f
    f=$(resolve_filter "$MODE" "$FILTER" build/tests/tccp_integration_tests integration) || return 0
    run_one "Integration Tests" tccp_integration_tests build/tests/tccp_integration_tests \
        test-results/integration.xml test-results/integration.log "$f"
}

# ── Main ─────────────────────────────────────────────────────────

RC=0

case "$SUITE" in
    unit)
        run_unit || RC=$?
        ;;
    integration)
        run_integration || RC=$?
        ;;
    all)
        run_unit || RC=$?
        echo ""
        run_integration || RC=$?
        ;;
    *)
        echo "Usage: ./scripts/test.sh [unit|integration|all] [failed|new|new+failed] [filter]"
        exit 1
        ;;
esac

exit $RC
