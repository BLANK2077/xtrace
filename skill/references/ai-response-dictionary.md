# xtrace AI Response Dictionary

This reference collects response fields an AI agent should expect when using `xtrace ai query`. Keep the main `SKILL.md` focused on request examples and workflow; update this file when action-specific response fields change.

## Stability Rule

The top-level envelope is the stable contract across all actions:

```text
ok/action/tool/session/summary/data/findings/suggested_next_actions/warnings/error/meta
```

Fields inside `summary`, `data`, and `findings` are action-specific. Treat this file as a practical dictionary, not a replacement for inspecting real output from the target xtrace build and daidir.

Recommended inspection pattern:

```bash
tools/xtrace-env ai query --json '<request>' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.keys()); print(d.get("summary",{})); print(d.get("data",{}).keys() if isinstance(d.get("data"),dict) else None)'
```

For production extraction, use `.get()` and defaults unless a field has been verified on the current output:

```bash
tools/xtrace-env ai query --json '<request>' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); edges=d.get("data",{}).get("dependency_edges",[]); print(len(edges))'
```

## Top-Level Envelope

| Field | Meaning | AI usage |
| --- | --- | --- |
| `ok` | Boolean success flag. | Check before reading action data. |
| `action` | Echoed action name. | Verify the response matches the request. |
| `tool` | Tool metadata. | Preserve in reports. |
| `session` | Session metadata when relevant. | Capture `id`, `dbdir`, `reused`, and health hints for follow-up requests. |
| `summary` | Compact action-specific summary. | Good for quick decisions and user-facing summaries. |
| `data` | Action-specific structured payload. | Use for evidence extraction and graph/path processing. |
| `findings` | List of detected facts or issues. | Use for trace/explain conclusions. |
| `suggested_next_actions` | Machine-readable follow-up hints. | Prefer these over inventing recovery steps. |
| `warnings` | Non-fatal warnings. | Preserve in debug reports. |
| `error` | Structured error object or null. | Read `error.code` and `recoverable`; do not parse stderr. |
| `meta` | Execution metadata. | Check `elapsed_ms` and `truncated` where present. |

## Common Nested Objects

### `error`

Typical fields:

```json
{
  "code": "SIGNAL_NOT_FOUND",
  "message": "signal top.ready not found",
  "recoverable": true,
  "candidates": [],
  "suggested_actions": []
}
```

AI usage:
- On `SIGNAL_NOT_FOUND`, run `signal.search` or `signal.canonicalize`.
- On `SESSION_UNHEALTHY`, run `session.doctor` or `session.ensure`.
- On parser or request errors, fix the request before retrying.

### `confidence`

Common values:

```text
high/medium/low/unknown
```

AI usage:
- `high`: exact signal, source location, and structured expression are available.
- `medium`: source location exists, but expression/control extraction is incomplete.
- `low`: statement-only, decompile-only, or string fallback evidence.
- `unknown`: xtrace could not assign reliable confidence.

Do not treat `low` or `medium` as proven causality. Use xwave to verify hypotheses on real waveforms.

### `assignment` / `expr_ast`

Typical AST nodes:

```text
signal/const/not/and/or/eq/neq/ternary/concat/select/part_select/call/unknown
```

AI usage:
- Use `rhs_signals` for candidate wave checks.
- Use `assignment.rhs` or `expr_ast` for hypothesis generation.
- Preserve `source` or decompiled text when AST contains `unknown`.

### `dependency_edges`

Common edge types:

```text
data_dependency/control_dependency/load_dependency/port_connection/interface_member
continuous_assignment/procedural_assignment/clocked_update/reset_assignment
case_branch/default_assignment/constant_assignment/statement_only
```

AI usage:
- Use data/control/load filters in graph/path queries when possible.
- Preserve `file`, `line`, `source`, `confidence`, and `relation` as evidence.

## Action Groups

### Session Actions

Actions:

```text
session.open/session.ensure/session.list/session.doctor/session.kill/session.close
```

Common payloads:
- `session.id`
- `session.dbdir`
- `session.reused`
- `summary.count`
- `summary.healthy`
- `data.sessions`

Extraction example:

```bash
tools/xtrace-env ai query --json '{"api_version":"xtrace.ai.v1","action":"session.list"}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print([s.get("id") for s in d.get("data",{}).get("sessions",[])])'
```

### Trace Actions

Actions:

```text
trace.driver/trace.load/trace.query
```

Common payloads:
- `summary.signal`
- `summary.mode`
- `summary.result_count`
- `summary.truncated`
- `summary.confidence`
- `data.results`
- `data.assignments`
- `data.rhs_signals`
- `data.dependency_edges`
- `data.control_dependencies`

Extraction example:

```bash
tools/xtrace-env ai query --json '{"api_version":"xtrace.ai.v1","action":"trace.driver","target":{"session_id":1},"args":{"signal":"top.u_dut.ready"},"limits":{"max_results":20}}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("data",{}).get("rhs_signals",[]))'
```

### Signal Actions

Actions:

```text
signal.resolve/signal.search/signal.canonicalize
```

Common payloads:
- `summary.query`
- `summary.count`
- `summary.truncated`
- `data.matches`
- `data.canonical`
- `data.aliases`
- `data.select`
- `data.ambiguous`

AI usage:
- Prefer `signal.canonicalize` when short names, array selects, or interface members may be involved.
- If `ambiguous` is true, ask for scope context or inspect candidates before tracing.

### Graph And Explanation Actions

Actions:

```text
trace.expand/trace.graph/trace.path/trace.explain/control.explain/source.context
```

Common payloads:
- `summary.root_signal`
- `summary.direction`
- `summary.truncated`
- `summary.path_count`
- `data.graph.nodes`
- `data.graph.edges`
- `data.paths`
- `data.explanations`
- `data.control_dependencies`
- `data.context`
- `data.enclosing`

AI usage:
- Check `summary.truncated` and `meta.truncated` before drawing conclusions.
- Use `trace.path` for yes/no causality reachability; use `trace.expand` or `trace.graph` for evidence collection.
- Use `source.context` before quoting source in reports.

### Expression And Semantic Actions

Actions:

```text
expr.normalize/procedural.assignment/sequential.update/fsm.explain/counter.explain
```

Common payloads:
- `summary.confidence`
- `summary.confidence_reason`
- `data.expr_ast`
- `data.procedural_assignment`
- `data.sequential_update`
- `data.fsm`
- `data.counter`
- `data.rules`
- `data.transitions`

AI usage:
- Prefer `expr.normalize` with `args.signal` over raw `args.expr`.
- Use `sequential.update` and `counter.explain` to generate candidate waveform checks such as reset, enable, increment, decrement, or hold.
- Use `fsm.explain` to generate state-transition hypotheses, then verify state and inputs with xwave.

### Port And Interface Actions

Actions:

```text
port.trace/instance.map/interface.resolve
```

Common payloads:
- `data.connections`
- `data.instances`
- `data.ports`
- `data.interfaces`
- `formal`
- `actual`
- `highconn`
- `lowconn`
- `direction`
- `modport`

AI usage:
- Use these actions when trace results stop at module boundaries or interface members.
- Confirm modport direction before treating a member as a driver.

### Batch

Action:

```text
batch
```

Common payloads:
- `data.responses`
- per-response envelope fields

AI usage:
- Use `continue_on_error` when later recovery steps can still run.
- Keep each child request bounded with limits.
