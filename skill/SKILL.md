---
name: xtrace
description: >
  Use when an AI agent needs structured JSON access to RTL causality facts through
  xtrace, including daidir sessions, driver/load tracing, signal discovery,
  dependency graphs, control/source context, procedural/sequential update rules,
  FSM/counter explanations, and module/interface/port connection tracing.
---

# xtrace AI JSON Interface

This skill is for AI agents using `xtrace ai ...` as a structured RTL causality API. Prefer the AI JSON entry point over the human CLI whenever you need machine-readable output, deterministic errors, confidence metadata, or multi-step debug evidence.

Human-oriented legacy CLI details were moved to [references/cli-reference.md](references/cli-reference.md). Load that file only when the user explicitly asks about non-AI command syntax.

## Entry Point

Use one of these forms:

```bash
tools/xtrace-env ai query request.json
tools/xtrace-env ai query -
tools/xtrace-env ai query --json '{"api_version":"xtrace.ai.v1","action":"trace.driver","target":{"dbdir":"simv.daidir","auto_ensure":true},"args":{"signal":"top.u_dut.ready"}}'
tools/xtrace-env ai schema
tools/xtrace-env ai actions
```

For scripted extraction, pipe JSON output into `python3` instead of parsing human text. This is the recommended way for AI agents to pull specific fields or compute custom statistics:

```bash
tools/xtrace-env ai query --json '{"api_version":"xtrace.ai.v1","action":"trace.driver","target":{"dbdir":"simv.daidir","auto_ensure":true},"args":{"signal":"top.u_dut.ready"},"limits":{"max_results":20}}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["ok"], d.get("summary",{}).get("confidence"))'
```

For larger summaries, keep the xtrace query bounded and do aggregation in Python:

```bash
tools/xtrace-env ai query --json '{"api_version":"xtrace.ai.v1","action":"trace.expand","target":{"session_id":1},"args":{"root_signal":"top.u_dut.ready","direction":"driver"},"limits":{"max_depth":3,"max_results":50}}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); g=d.get("data",{}).get("graph",{}); print(len(g.get("nodes",[])), len(g.get("edges",[])))'
```

Request envelope:

```json
{
  "api_version": "xtrace.ai.v1",
  "request_id": "optional-id",
  "action": "trace.driver",
  "target": {
    "dbdir": "/path/to/simv.daidir",
    "auto_ensure": true
  },
  "args": {},
  "limits": {
    "max_results": 50,
    "max_depth": 3,
    "max_paths": 10,
    "timeout_ms": 5000
  },
  "output": {
    "include_source": true,
    "include_control_dependencies": true,
    "include_expr": true,
    "include_graph": false,
    "verbosity": "compact"
  }
}
```

Response envelope always contains:

```text
ok/action/tool/session/summary/data/findings/suggested_next_actions/warnings/error/meta
```

Only the top-level response envelope is stable across all actions. Field names inside `summary`, `data`, and `findings` are action-specific and may differ by action. Do not guess detailed keys from memory. For a field dictionary and extraction guidance, see [references/ai-response-dictionary.md](references/ai-response-dictionary.md). For exact fields on a specific build and daidir, run `tools/xtrace-env ai schema` when available, or issue a small bounded query and inspect the returned JSON before writing extraction code.

AI usage rules:
- Start with `session.ensure` for repeated work, then use `target.session_id`.
- For one-shot queries, use `target.dbdir + auto_ensure:true`.
- Always inspect `ok` and `error.code`; do not parse human text.
- Prefer `python3 -c 'import json,sys; ...'` pipelines for extracting fields or computing statistics from `xtrace ai query` output.
- Treat `confidence:"low"` or `confidence:"medium"` as RTL evidence that needs human review or xwave verification before calling a hypothesis proven.
- Prefer `trace.driver`, `trace.load`, or `trace.query` for direct evidence; use graph/path/explain actions after you have a concrete root signal.
- Use external `rg`/grep on RTL source to discover candidate names; xtrace only resolves exact paths.
- Use `limits.max_results/max_depth/max_paths` for graph and broad trace actions.
- Use `batch` for multi-step debug plans to reduce repeated session setup.
- Runtime state lives under `~/.xtrace/`: `registry.json`, `registry.lock`, and `sessions/<sid>/session.json`, `socket`, `debug.log`. Older top-level `~/.xtrace.registry` is a legacy migration input only.
- For session startup failures, rerun with `--debug` or `XTRACE_DEBUG=1` and inspect `~/.xtrace/sessions/<sid>/debug.log`.

## Session Actions

### `session.open`

Open a daidir-backed session. Use `session.ensure` for most agent flows unless you explicitly need a fresh open.

```json
{"api_version":"xtrace.ai.v1","action":"session.open","target":{"dbdir":"/path/to/simv.daidir"}}
```

### `session.ensure`

Open or reuse a healthy session. Use this once at the start of a multi-step trace.

```json
{"api_version":"xtrace.ai.v1","action":"session.ensure","target":{"dbdir":"/path/to/simv.daidir"}}
```

### `session.list`

List known sessions. Use before reusing an existing session.

```json
{"api_version":"xtrace.ai.v1","action":"session.list"}
```

### `session.doctor`

Check registry, daemon, socket, and daidir health.

```json
{"api_version":"xtrace.ai.v1","action":"session.doctor","target":{"session_id":1}}
```

### `session.kill`

Stop one session or all sessions.

```json
{"api_version":"xtrace.ai.v1","action":"session.kill","args":{"id":"all"}}
```

### `session.close`

Close a session alias; use when the action model calls for close rather than kill.

```json
{"api_version":"xtrace.ai.v1","action":"session.close","target":{"session_id":1}}
```

## Trace Actions

### `trace.driver`

Find RTL drivers for one signal. Use for root-cause candidates such as why `ready` is low.

```json
{"api_version":"xtrace.ai.v1","action":"trace.driver","target":{"session_id":1},"args":{"signal":"top.u_dut.ready","no_statement_only":true},"limits":{"max_results":20},"output":{"include_expr":true,"include_source":true}}
```

### `trace.load`

Find RTL loads or uses of one signal. Use for fanout impact and where a signal is consumed.

```json
{"api_version":"xtrace.ai.v1","action":"trace.load","target":{"session_id":1},"args":{"signal":"top.u_dut.valid","role":"rhs_use"},"limits":{"max_results":20}}
```

### `trace.query`

Mode-selecting wrapper for driver/load. Use when an agent has a variable direction.

```json
{"api_version":"xtrace.ai.v1","action":"trace.query","target":{"session_id":1},"args":{"mode":"driver","signal":"top.u_dut.ready"},"limits":{"max_results":20}}
```

## Signal Actions

### `signal.resolve`

Resolve an exact or near-exact signal path.

```json
{"api_version":"xtrace.ai.v1","action":"signal.resolve","target":{"session_id":1},"args":{"signal":"top.u_dut.ready"}}
```

### `signal.canonicalize`

Normalize a signal into canonical path metadata, select/array hints, aliases, and ambiguity information.

```json
{"api_version":"xtrace.ai.v1","action":"signal.canonicalize","target":{"session_id":1},"args":{"signal":"ready"},"limits":{"max_results":20}}
```

## Graph And Explanation Actions

### `trace.expand`

Recursively expand driver/load causality from a root signal. Use strict limits.

```json
{"api_version":"xtrace.ai.v1","action":"trace.expand","target":{"session_id":1},"args":{"root_signal":"top.u_dut.ready","direction":"driver","dependency_types":["data","control"]},"limits":{"max_depth":3,"max_results":80}}
```

### `trace.graph`

Return graph-shaped causality data for a root signal.

```json
{"api_version":"xtrace.ai.v1","action":"trace.graph","target":{"session_id":1},"args":{"root_signal":"top.u_dut.ready","direction":"driver"},"limits":{"max_depth":3,"max_results":80}}
```

### `trace.path`

Check whether a causality path exists between two signals.

```json
{"api_version":"xtrace.ai.v1","action":"trace.path","target":{"session_id":1},"args":{"from_signal":"top.u_dut.fifo_full","to_signal":"top.u_dut.ready","dependency_types":["data","control"]},"limits":{"max_depth":5,"max_paths":10}}
```

### `trace.explain`

Produce a compact AI-facing causality explanation and suggested waveform verification steps.

```json
{"api_version":"xtrace.ai.v1","action":"trace.explain","target":{"session_id":1},"args":{"signal":"top.u_dut.ready","mode":"why_can_change","direction":"driver"},"limits":{"max_depth":3,"max_results":20}}
```

### `control.explain`

Explain if/case/default control dependencies for a target signal.

```json
{"api_version":"xtrace.ai.v1","action":"control.explain","target":{"session_id":1},"args":{"signal":"top.u_dut.ready","include_nested":true},"limits":{"max_results":20}}
```

### `source.context`

Return source lines around a file/line and infer the enclosing module/always/if/case/begin block when possible.

```json
{"api_version":"xtrace.ai.v1","action":"source.context","args":{"file":"/path/to/rtl.sv","line":88,"context_lines":8,"include_enclosing_block":true}}
```

## Expression And Semantic Actions

### `expr.normalize`

Normalize an expression to an AST. Prefer `args.signal` so xtrace can use the real NPI assignment; `args.expr` is a low-confidence string fallback.

```json
{"api_version":"xtrace.ai.v1","action":"expr.normalize","target":{"session_id":1},"args":{"signal":"top.u_dut.ready"}}
```

```json
{"api_version":"xtrace.ai.v1","action":"expr.normalize","args":{"expr":"valid && !ready"}}
```

### `procedural.assignment`

Extract procedural assignments for a target signal, including default assignments, branch assignments, active conditions, source location, and dependency edges.

```json
{"api_version":"xtrace.ai.v1","action":"procedural.assignment","target":{"session_id":1},"args":{"signal":"top.u_dut.ready"},"limits":{"max_results":50}}
```

### `sequential.update`

Explain register update rules, including clock/reset hints and reset/increment/decrement/hold/update classifications.

```json
{"api_version":"xtrace.ai.v1","action":"sequential.update","target":{"session_id":1},"args":{"signal":"top.u_dut.count"},"limits":{"max_results":50}}
```

### `fsm.explain`

Explain state register transitions and condition/source evidence.

```json
{"api_version":"xtrace.ai.v1","action":"fsm.explain","target":{"session_id":1},"args":{"signal":"top.u_dut.state_q"},"limits":{"max_results":50}}
```

### `counter.explain`

Explain counter-like update rules and whether the target appears counter-like.

```json
{"api_version":"xtrace.ai.v1","action":"counter.explain","target":{"session_id":1},"args":{"signal":"top.u_dut.active_count"},"limits":{"max_results":50}}
```

## Port And Interface Actions

### `port.trace`

Trace formal/actual highconn/lowconn connection chains for a signal or port.

```json
{"api_version":"xtrace.ai.v1","action":"port.trace","target":{"session_id":1},"args":{"signal":"top.u_mid.u_leaf.out"},"limits":{"max_depth":5,"max_results":50}}
```

### `instance.map`

Return instance-to-module and port mapping information.

```json
{"api_version":"xtrace.ai.v1","action":"instance.map","target":{"session_id":1},"args":{"instance":"top.u_mid.u_leaf"},"limits":{"max_results":100}}
```

### `interface.resolve`

Resolve interface/modport/member relationships, including pass-through and member select hints when available.

```json
{"api_version":"xtrace.ai.v1","action":"interface.resolve","target":{"session_id":1},"args":{"signal":"top.if0.valid"},"limits":{"max_results":50}}
```

## Batch Action

### `batch`

Run multiple AI requests in one ordered call. Use `continue_on_error` for debug plans where later recovery steps can still be useful.

```json
{"api_version":"xtrace.ai.v1","action":"batch","target":{"session_id":1},"args":{"mode":"continue_on_error","requests":[{"action":"trace.driver","args":{"signal":"top.u_dut.ready"}},{"action":"trace.driver","args":{"signal":"top.u_dut.fifo_full"}}]}}
```
