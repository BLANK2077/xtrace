---
name: xtrace
description: >
  Use when working with xtrace, an NPI-based CLI for tracing RTL signal drivers,
  loads, and control dependencies from a VCS simv.daidir database. Also use when
  the user mentions xtrace open/session/doctor/driver/load, daidir session
  health, NPI signal tracing, or debugging xtrace daemon lifecycle behavior.
---

# xtrace - NPI RTL signal tracing CLI

## Overview

xtrace is a Synopsys NPI based command-line tool for loading a VCS `simv.daidir`
database and querying signal driver/load relationships without launching the full
Verdi GUI. The same `xtrace` binary acts as both CLI client and background daemon.

Use Chinese with the user by default.

## Environment

- `VERDI_HOME` must point to a Verdi installation with NPI headers and libraries.
- Prefer `tools/xtrace-env` for runtime commands because it sets `LD_LIBRARY_PATH`.
- Build from the repo root with `make`; use `make clean && make` after changing
  link inputs, session/server code, or command routing.

```bash
export VERDI_HOME=/path/to/verdi/V-2023.12-SP2
make clean && make
tools/xtrace-env open -dbdir /path/to/simv.daidir
```

## Core workflow

```bash
# 1. Load a VCS daidir. The path must end in .daidir.
tools/xtrace-env open -dbdir /path/to/simv.daidir

# 2. Check session health.
tools/xtrace-env session list
tools/xtrace-env session doctor -s 1 -json

# 3. Query signal relationships.
tools/xtrace-env driver top.u_dut.signal -s 1
tools/xtrace-env load top.u_dut.signal -s 1

# 4. Clean up.
tools/xtrace-env session kill 1
```

## Important behavior

- `open` only supports `-dbdir <*.daidir> [other NPI options]`.
- xtrace canonicalizes the daidir path and records `mtime/size/dev/inode`.
- Opening the same canonical daidir reuses an existing healthy session.
- `session doctor -s <sid> [-json]` is the canonical health check.
- Health status may be `healthy`, `registry_missing`, `dbdir_missing`,
  `dbdir_changed`, `process_exited`, `socket_missing`, `connect_failed`, or
  `ping_failed`.

## Project map

- `src/main.cpp`: top-level CLI dispatch and `--server` entry.
- `src/commands/`: client-side command parsing and help output.
- `src/session/`: registry, daemon spawn, canonical daidir metadata, health checks.
- `src/client/`: Unix socket client and response handling.
- `src/server/`: daemon loop, NPI design load, driver/load command handlers.
- `src/control_dep/`: control dependency tracing fallback logic.
- `src/protocol/`: shared socket protocol constants.

## Guardrails

- Do not reintroduce `open -sv`; published xtrace loads through `-dbdir`.
- Do not treat a socket file alone as healthy; health requires registry, daidir
  metadata, live process, connect, and `PING/PONG`.
- Keep daemon lifecycle aligned with xwave-pkg: child server must detach from the
  short-lived CLI process with `setsid()` and ignore `SIGHUP`.
- Do not hardcode NPI constants from memory; inspect `$VERDI_HOME/share/NPI/inc`
  or `$VERDI_HOME/share/NPI/L1/C/inc` when needed.
- Keep generated outputs (`xtrace`, `*.o`, `xtraceLog/`, local daidir/csrc/test
  assets) out of the release repository.
