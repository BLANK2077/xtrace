#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UART_DB="${UART_DB:-$ROOT_DIR/uart/simv.daidir}"
IFACE_DB="${IFACE_DB:-/home/yian/worken/mod_port_trace/test/testcases/interface_port/simv.daidir}"
P3_DB="${P3_DB:-$ROOT_DIR/regression/out/p3_semantics/simv.daidir}"
TMP_HOME="$(mktemp -d)"

cleanup() {
  HOME="$TMP_HOME" "$ROOT_DIR/tools/xtrace-env" ai query --json \
    '{"api_version":"xtrace.ai.v1","action":"session.kill","args":{"id":"all"}}' >/dev/null 2>&1 || true
  rm -rf "$TMP_HOME"
}
trap cleanup EXIT

require_db() {
  local path="$1"
  if [[ ! -d "$path" ]]; then
    echo "missing regression database: $path" >&2
    exit 1
  fi
}

build_p3_db() {
  if [[ -d "$P3_DB" ]]; then
    return
  fi
  if ! command -v vcs >/dev/null 2>&1; then
    echo "missing regression database and vcs is unavailable: $P3_DB" >&2
    exit 1
  fi
  local out_dir
  out_dir="$(dirname "$P3_DB")"
  rm -rf "$out_dir"
  mkdir -p "$out_dir"
  (
    cd "$out_dir"
    vcs -full64 -sverilog -kdb -debug_access+all \
      "$ROOT_DIR/regression/fixtures/p3_semantics/p3_semantics.sv" \
      -top p3_sem_top -o simv >/tmp/xtrace_p3_vcs.log 2>&1
  ) || {
    cat /tmp/xtrace_p3_vcs.log >&2
    exit 1
  }
}

query() {
  HOME="$TMP_HOME" "$ROOT_DIR/tools/xtrace-env" ai query --json "$1"
}

check_json() {
  python3 -c '
import json
import sys

payload = json.load(sys.stdin)
expr = sys.argv[1]
ns = {"d": payload}
if not eval(expr, {}, ns):
    print(json.dumps(payload, indent=2), file=sys.stderr)
    raise SystemExit(f"check failed: {expr}")
' "$@"
}

require_db "$UART_DB"
require_db "$IFACE_DB"
build_p3_db
require_db "$P3_DB"

"$ROOT_DIR/tools/xtrace-env" ai actions | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert "trace.driver" in d["implemented"]
assert "port.trace" in d["implemented"]
assert "sequential.update" in d["implemented"]
assert "counter.explain" in d["implemented"]
assert not d["experimental"]
'

"$ROOT_DIR/tools/xtrace-env" ai schema | python3 -c 'import json,sys; json.load(sys.stdin)'

query "{\"api_version\":\"xtrace.ai.v1\",\"action\":\"trace.driver\",\"target\":{\"dbdir\":\"$UART_DB\",\"auto_ensure\":true},\"args\":{\"signal\":\"uart_16550.RXDin\"},\"limits\":{\"max_results\":10}}" \
  | check_json 'd["ok"] and d["data"]["assignment"]["rhs"]["op"] == "ternary" and len(d["data"]["dependency_edges"]) >= 2 and d["summary"]["confidence"] in ("high","medium")'

query '{"api_version":"xtrace.ai.v1","action":"trace.expand","target":{"session_id":1},"args":{"root_signal":"uart_16550.RXDin","direction":"driver"},"limits":{"max_depth":2,"max_nodes":20,"max_edges":50}}' \
  | check_json 'd["ok"] and d["summary"]["node_count"] >= 2 and d["summary"]["edge_count"] >= 1 and d["meta"]["truncated"] is False'

query '{"api_version":"xtrace.ai.v1","action":"trace.path","target":{"session_id":1},"args":{"from_signal":"uart_16550.loopback","to_signal":"uart_16550.RXDin","direction":"driver"},"limits":{"max_depth":2}}' \
  | check_json 'd["ok"] and d["summary"]["found"] is True and d["summary"]["path_count"] >= 1'

query '{"api_version":"xtrace.ai.v1","action":"signal.canonicalize","target":{"session_id":1},"args":{"signal":"uart_16550.RXDin"}}' \
  | check_json 'd["ok"] and d["data"]["canonical"].endswith("RXDin")'

query '{"api_version":"xtrace.ai.v1","action":"source.context","args":{"file":"'"$ROOT_DIR"'/uart/uart_16550.sv","line":164,"context_lines":2}}' \
  | check_json 'd["ok"] and len(d["data"]["context"]) == 5 and any(x["hit"] for x in d["data"]["context"]) and d["data"]["enclosing"]["type"] != "unknown"'

query '{"api_version":"xtrace.ai.v1","action":"expr.normalize","target":{"session_id":1},"args":{"signal":"uart_16550.RXDin"},"limits":{"max_results":10}}' \
  | check_json 'd["ok"] and d["summary"]["source"] == "npi_trace_assignment" and d["data"]["expr"]["op"] == "ternary"'

query '{"api_version":"xtrace.ai.v1","action":"expr.normalize","args":{"expr":"valid && !ready"}}' \
  | check_json 'd["ok"] and d["summary"]["source"] == "string_fallback" and d["summary"]["confidence"] == "low"'

query "{\"api_version\":\"xtrace.ai.v1\",\"action\":\"instance.map\",\"target\":{\"dbdir\":\"$IFACE_DB\",\"auto_ensure\":true},\"args\":{\"path\":\"test_top.uut\"}}" \
  | check_json 'd["ok"] and d["summary"]["port_count"] >= 8 and d["data"]["port_count"] >= 8'

query '{"api_version":"xtrace.ai.v1","action":"interface.resolve","target":{"session_id":2},"args":{"path":"test_top.bus_if_inst"}}' \
  | check_json 'd["ok"] and d["data"]["object"]["type"] == "interface" and d["summary"]["port_count"] >= 1'

query '{"api_version":"xtrace.ai.v1","action":"port.trace","target":{"session_id":2},"args":{"path":"test_top.uut"},"limits":{"max_results":3}}' \
  | check_json 'd["ok"] and d["summary"]["port_count"] == 3 and d["summary"]["truncated"] is True'

query "{\"api_version\":\"xtrace.ai.v1\",\"action\":\"procedural.assignment\",\"target\":{\"dbdir\":\"$P3_DB\",\"auto_ensure\":true},\"args\":{\"signal\":\"p3_sem_top.u_mid.u_leaf.out\"},\"limits\":{\"max_results\":30}}" \
  | check_json 'd["ok"] and d["summary"]["assignment_count"] >= 1 and d["data"]["procedural_assignment"]["branch_assignments"]'

query '{"api_version":"xtrace.ai.v1","action":"sequential.update","target":{"session_id":3},"args":{"signal":"p3_sem_top.u_mid.u_leaf.count"},"limits":{"max_results":30}}' \
  | check_json 'd["ok"] and d["summary"]["rule_count"] >= 1 and any(r["kind"] in ("reset","increment","decrement","hold") for r in d["data"]["sequential_update"]["rules"])'

query '{"api_version":"xtrace.ai.v1","action":"fsm.explain","target":{"session_id":3},"args":{"signal":"p3_sem_top.u_mid.u_leaf.state_q"},"limits":{"max_results":30}}' \
  | check_json 'd["ok"] and d["summary"]["transition_count"] >= 1 and d["data"]["fsm"]["transitions"]'

query '{"api_version":"xtrace.ai.v1","action":"counter.explain","target":{"session_id":3},"args":{"signal":"p3_sem_top.u_mid.u_leaf.count"},"limits":{"max_results":30}}' \
  | check_json 'd["ok"] and d["summary"]["counter_like"] is True and any(r["kind"] == "increment" for r in d["data"]["counter"]["rules"])'

query '{"api_version":"xtrace.ai.v1","action":"batch","args":{"mode":"continue_on_error","requests":[{"api_version":"xtrace.ai.v1","action":"trace.driver","target":{"session_id":1},"args":{"signal":"uart_16550.TXD"}},{"api_version":"xtrace.ai.v1","action":"signal.search","target":{"session_id":1},"args":{"query":"RXD"},"limits":{"max_results":5}}]}}' \
  | check_json 'd["ok"] and d["summary"]["count"] == 2'

HOME="$TMP_HOME" "$ROOT_DIR/tools/xtrace-env" driver uart_16550.RXDin -s 1 -json \
  | check_json 'd["ok"] and "dependency_edges" not in d and d.get("result_count", 0) >= 1'

echo "xtrace AI semantics regression passed"
