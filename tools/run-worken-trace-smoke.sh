#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKEN_CASE_DIR="${WORKEN_CASE_DIR:-/home/yian/worken/mod_port_trace/test/testcases/basic_output}"
BUILD_DIR="${BUILD_DIR:-/tmp/xtrace_worken_trace_smoke}"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cd "$BUILD_DIR"
VCS_TARGET_ARCH="${VCS_TARGET_ARCH:-linux64}" \
vcs -full64 -sverilog -kdb -lca \
    "$WORKEN_CASE_DIR/test_basic.v" \
    "$WORKEN_CASE_DIR/test_top.v" \
    -o simv

cd "$ROOT_DIR"
open_output="$(tools/xtrace-env open -dbdir "$BUILD_DIR/simv.daidir")"
printf '%s\n' "$open_output"

sid="$(printf '%s\n' "$open_output" | sed -n 's/.*\[Session \([0-9][0-9]*\)\].*/\1/p' | tail -n 1)"
if [[ -z "$sid" ]]; then
    echo "Error: failed to parse session id" >&2
    exit 1
fi

tools/xtrace-env session doctor -s "$sid"
tools/xtrace-env driver test_top.uut.data_out -s "$sid" -json
tools/xtrace-env load test_top.data_in -s "$sid" -json
tools/xtrace-env driver test_top.uut.data_out -s "$sid"
tools/xtrace-env load test_top.data_in -s "$sid"
tools/xtrace-env session kill "$sid"
