English | [中文](README.zh.md)

# XTrace - NPI Signal Tracing Tool

XTrace is a Synopsys NPI (Native Programming Interface) based signal tracing CLI. It uses a client-server architecture and supports concurrent session management, signal driver/load tracing, and control dependency analysis.

## Features

### Session Management

- **Concurrent sessions**: manage multiple design databases, with each session loading one database independently.
- **Automatic session IDs**: no manual ID allocation is required.
- **Session lifecycle management**: create (`open`), list (`session list`), and close (`session kill` / `close`) sessions.
- **Agent-friendly entry points**: `session ensure -dbdir ... [-json]` reuses or creates a healthy session, and `query` combines ensure + trace in one command.
- **Stale session cleanup**: detects and removes abnormal server sessions.
- **Session health diagnostics**: `session doctor -s <sid> [-json]` distinguishes registry, process, socket, connect, and PING failures.
- **Daidir fingerprint checking**: `open` only accepts `-dbdir <*.daidir>` and records canonical path, mtime, size, dev, and inode.
- **Targeted commands**: use `-s <session_id>` to send commands to a specific session.

### Signal Tracing (Driver / Load)

- **`driver <signal>`**: trace signal drivers and report source signal names with RTL source locations.
- **`load <signal>`**: trace signal loads and report load-side signal names with RTL source locations.
- **JSON output**: `driver/load ... -json` emits structured `ok/query/mode/results/control_dependencies` plus summary fields.
- **Result filters**: supports `--limit <N>`, `--role <role>`, and `--no-statement-only`.
- **Signal discovery**: `signal resolve/search` resolves full signal names or searches candidate signals.
- **Source locations**: each driver/load result includes file, line, and source text when available.
- **Interface support**: handles SystemVerilog interface member references (`npiOperation`) and continuous assignments (`npiContAssign`).

### Control Dependency Tracing

- When NPI returns no direct result for procedural assignments in `always` blocks, XTrace falls back to AST traversal to extract control conditions for target signals.
- Supports control dependencies from `if`, `case`, `while`, `wait`, and `always` constructs.

## LSF / bsub Usage

XTrace uses a local daemon per session. The daemon is reached through a Unix domain socket, and session health checks use local PID and `/proc` state. Because of this, `open`, `session ensure`, `query`, `driver`, `load`, and `session kill` must run on the same machine.

In chip-company LSF environments, avoid submitting XTrace commands to a normal queue that may dispatch each command to a different host. The recommended setup is to ask IT to create a dedicated queue that contains exactly one suitable machine for XTrace/XWave-style NPI tools. Then submit all XTrace commands to that queue:

```bash
bsub -q <xtrace_queue> -I "cd <workdir> && tools/xtrace-env session ensure -dbdir /path/to/simv.daidir -json"
bsub -q <xtrace_queue> -I "cd <workdir> && tools/xtrace-env query -dbdir /path/to/simv.daidir --driver top.sig -json"
bsub -q <xtrace_queue> -I "cd <workdir> && tools/xtrace-env session kill 1"
```

The dedicated machine should have access to the shared daidir path and a consistent Verdi/NPI/license environment. If a dedicated single-host queue is not available, the next simplest option is to use `bsub -m <host>` to pin all commands to one host.

If fixed-machine operation is still not acceptable, the project architecture needs additional work, such as a TCP daemon, automatic remote command forwarding, or a no-daemon single-command mode. Those options are more complex and should be treated as code changes rather than normal usage.

## Build

Set `VERDI_HOME` first:

```bash
export VERDI_HOME=/path/to/verdi/V-2023.12-SP2
export LD_LIBRARY_PATH=$VERDI_HOME/share/NPI/lib/LINUX64:$LD_LIBRARY_PATH
```

Then build:

```bash
cd /home/yian/xtrace
make
```

At runtime, prefer `tools/xtrace-env`; it sets the NPI runtime library path from `VERDI_HOME`:

```bash
tools/xtrace-env help
```

## Usage

### Session Management

```bash
# Create a new session by loading a VCS daidir.
tools/xtrace-env open -dbdir /path/to/simv.daidir
[Session 1] Database loaded.

# Ensure a healthy session for the same daidir. This is recommended for scripts and agents.
tools/xtrace-env session ensure -dbdir /path/to/simv.daidir -json

# List all sessions.
tools/xtrace-env session list

# Diagnose a session.
tools/xtrace-env session doctor -s 1
tools/xtrace-env session doctor -s 1 -json

# Kill a specific session.
tools/xtrace-env session kill 1

# Kill all sessions.
tools/xtrace-env session kill all

# Close the latest session.
tools/xtrace-env close
```

### Signal Tracing

```bash
# Trace drivers.
tools/xtrace-env driver test_top.uut.bus.ready -s 1
tools/xtrace-env driver test_top.uut.bus.ready -s 1 -json

# Trace loads.
tools/xtrace-env load test_top.valid_in -s 1
tools/xtrace-env load test_top.valid_in -s 1 -json

# Filter trace results.
tools/xtrace-env driver test_top.uut.bus.ready -s 1 -json --limit 5
tools/xtrace-env load test_top.valid_in -s 1 -json --role rhs_use --no-statement-only
```

`driver/load -json` uses the same trace engine as text output. A typical JSON payload is:

```json
{
  "ok": true,
  "query": "test_top.uut.data_out",
  "mode": "driver",
  "result_count": 1,
  "truncated": false,
  "roles": ["driver"],
  "files": ["/path/to/test_basic.v"],
  "has_statement_only": false,
  "results": [
    {
      "signal": "test_top.data_in",
      "role": "driver",
      "file": "/path/to/test_basic.v",
      "line": 14,
      "source": "assign data_out = data_in;",
      "resolution": "signal"
    }
  ],
  "control_dependencies": []
}
```

### Recommended AI Agent Entry Point

```bash
# Ensure session + driver trace in one command.
tools/xtrace-env query -dbdir /path/to/simv.daidir --driver test_top.uut.bus.ready -json --limit 10

# Ensure session + load trace in one command.
tools/xtrace-env query -dbdir /path/to/simv.daidir --load test_top.valid_in -json --role rhs_use
```

`query -json` returns both `session` and `trace` sections. On failure it reports `ok=false`, `status`, and `message`, which makes it suitable for agent decision loops.

### AI JSON Interface

For AI agents and xdebug-style orchestration, prefer the unified JSON entry point:

```bash
tools/xtrace-env ai query request.json
tools/xtrace-env ai query -
tools/xtrace-env ai query --json '{"api_version":"xtrace.ai.v1","action":"trace.driver","target":{"dbdir":"/path/to/simv.daidir","auto_ensure":true},"args":{"signal":"top.u_dut.ready"}}'
tools/xtrace-env ai schema
tools/xtrace-env ai actions
```

The AI response envelope always contains `ok/action/tool/session/summary/data/findings/suggested_next_actions/warnings/error/meta`. Action-specific payloads live under `summary` and `data`; use `ok` and `error.code` instead of parsing human text.

Implemented AI actions include:

- `session.open`, `session.ensure`, `session.list`, `session.doctor`, `session.kill`, `session.close`
- `trace.driver`, `trace.load`, `trace.query`
- `signal.resolve`, `signal.search`, `signal.canonicalize`
- `trace.expand`, `trace.graph`, `trace.path`, `trace.explain`, `control.explain`, `source.context`
- `expr.normalize`, `procedural.assignment`, `sequential.update`, `fsm.explain`, `counter.explain`, `port.trace`, `instance.map`, `interface.resolve`
- `batch`

AI trace responses add dependency-oriented fields such as `rhs_signals`, `dependency_edges`, `assignment`, `confidence`, and `confidence_reason` without changing the legacy `driver/load/query -json` output.

### Signal Discovery

```bash
# Resolve a full signal name.
tools/xtrace-env signal resolve test_top.uut.bus.ready -s 1 -json

# Search candidate signals by leaf name or substring.
tools/xtrace-env signal search ready -s 1 -json --limit 20
```

JSON output includes `ok/query/matches/count/truncated`. Missing signals return non-zero with `status=not_found`.

### Session Doctor Exit Codes

`session doctor` is the scriptable health-check entry point:

- Healthy sessions return `0` with status `healthy`.
- Unhealthy sessions return non-zero.
- Missing `-s <sid>` or invalid session IDs return non-zero.
- JSON output includes `session_id`, `healthy`, `status`, `message`, `pid`, `socket_path`, `design_file`, `dbdir_path`, and daidir metadata.

Status values:

- `dbdir_missing`: the daidir path is missing or is not a directory.
- `dbdir_changed`: daidir mtime/size/dev/inode differs from the metadata captured at open time.
- `registry_missing`: the session is not in the registry.
- `process_exited`: the server process no longer exists.
- `socket_missing`: the socket file is missing.
- `connect_failed`: the socket exists but cannot be connected.
- `ping_failed`: the server did not respond to `PING`.
- `healthy`: registry, process, socket, connect, and `PING/PONG` checks all passed.

## Project Layout

```text
xtrace/
├── src/
│   ├── main.cpp                    # CLI entry point and command routing
│   ├── commands/                   # Command implementations
│   │   ├── cmd_session.cpp         # Session management commands
│   │   └── cmd_trace.cpp           # Driver/load tracing commands
│   ├── session/                    # Session management core
│   │   ├── session_manager.cpp
│   │   └── session_registry.cpp
│   ├── client/                     # Client communication
│   │   └── client.cpp
│   ├── server/                     # NPI server logic
│   │   └── server.cpp
│   ├── trace/                      # Driver/load trace engine
│   │   ├── trace_engine.cpp
│   │   └── trace_engine.h
│   ├── signal/                     # Signal resolve/search
│   │   ├── signal_finder.cpp
│   │   └── signal_finder.h
│   ├── control_dep/                # Control dependency analysis
│   │   ├── control_dep.cpp
│   │   └── control_dep.h
│   └── protocol/
│       └── protocol.h              # Protocol definitions
├── tools/
│   └── xtrace-env                  # Runtime wrapper for VERDI_HOME/LD_LIBRARY_PATH
├── skill/
│   └── SKILL.md                    # Agent skill for xtrace workflows
├── Makefile
└── README.md
```

## Dependencies

- Synopsys Verdi `V-2023.12-SP2+`
- NPI libraries (`libNPI.so`, `libnpiL1.so`)
- C++11 compiler whose libstdc++ ABI matches the selected Verdi/NPI libraries

### Tested Tool Versions

- Verdi: `V-2023.12-SP2` for `linux64`
- VCS: `V-2023.12-SP2_Full64`
- G++: GCC `8.5.0`

Notes:

- Building test daidir databases with VCS on this machine requires `VCS_TARGET_ARCH=linux64`.
- NPI L1 FSDB/RTL C++ APIs use `std::string`, so the C++ ABI must match Synopsys `libnpiL1.so`.
- Verdi 2020 NPI libraries can be built directly with GCC 4.8.
- Verdi 2023 NPI libraries export new-ABI symbols such as `std::__cxx11::basic_string`, so GCC 5+ is required. If you see undefined references involving `std::string` / `std::__cxx11::basic_string`, upgrade g++ and avoid `-D_GLIBCXX_USE_CXX11_ABI=0`.

## Known Limitations

- XTrace currently uses the **SV Language Model** at RTL level. Some procedural assignment cases unsupported by NPI may still return empty driver/load results.
- Generic-name tracing does not yet remap all module port reference paths. Some deep interface driver sources may require the module port reference path for accurate tracing.
