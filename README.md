# XTrace - NPI Signal Tracing Tool

XTrace 是一个基于 Synopsys NPI (Native Programming Interface) 的信号追踪 CLI 工具，采用 Client-Server 架构，支持多 Session 并发管理、信号驱动/负载追踪以及控制依赖分析。

## 功能特性

### Session 管理
- **多 Session 并发** — 同时管理多个设计数据库，每个 Session 独立加载设计
- **自动分配 Session ID** — 无需手动指定 ID
- **Session 生命周期管理** — 创建 (`open`)、列出 (`session list`)、关闭 (`session kill` / `close`)
- **Agent 友好入口** — `session ensure -dbdir ... [-json]` 可复用或创建健康 Session，`query` 可一次完成 ensure + trace
- **僵死 Session 自动清理** — 自动检测并回收异常退出的 Session
- **Session 健康诊断** — `session doctor -s <sid> [-json]` 区分 registry 缺失、进程退出、socket 缺失、连接失败和 PING 失败
- **Daidir 指纹校验** — `open` 只接受 `-dbdir <*.daidir>`，并记录 canonical path、mtime、size、dev、inode
- **定向命令** — 支持通过 `-s <session_id>` 向指定 Session 发送命令

### 信号追踪 (Driver / Load)
- **`driver <signal>`** — 追踪信号的驱动源，输出驱动信号名及对应的 RTL 源代码行
- **`load <signal>`** — 追踪信号的负载，输出负载信号名及对应的 RTL 源代码行
- **JSON 输出** — `driver/load ... -json` 输出结构化 `ok/query/mode/results/control_dependencies` 和摘要字段
- **结果过滤** — 支持 `--limit <N>`、`--role <role>`、`--no-statement-only`
- **信号发现** — `signal resolve/search` 用于解析完整信号或搜索候选信号
- **源代码定位** — 每个 driver/load 结果都附带源文件名和行号，以及对应源代码内容
- **Interface 支持** — 正确处理 SystemVerilog interface 成员引用 (`npiOperation`) 和连续赋值 (`npiContAssign`)

### 控制依赖追踪
- 当 NPI 对 `always` 块中的过程赋值返回空结果时，自动通过 AST 遍历提取控制目标信号的条件表达式
- 支持 `if`/`case`/`while`/`wait`/`always` 等结构中的控制依赖分析

## 编译

确保已设置环境变量 `VERDI_HOME`：

```bash
export VERDI_HOME=/path/to/verdi/V-2023.12-SP2
export LD_LIBRARY_PATH=$VERDI_HOME/share/NPI/lib/LINUX64:$LD_LIBRARY_PATH
```

然后编译：

```bash
cd /home/yian/xtrace
make
```

运行时推荐使用 `tools/xtrace-env`，它会根据 `VERDI_HOME` 自动设置 NPI 动态库路径：

```bash
tools/xtrace-env help
```

## 使用

### Session 管理

```bash
# 创建新 Session (加载 VCS daidir)
tools/xtrace-env open -dbdir /path/to/simv.daidir
[Session 1] Database loaded.

# 确保同一个 daidir 有健康 Session；适合脚本和 agent 使用
tools/xtrace-env session ensure -dbdir /path/to/simv.daidir -json

# 列出所有 Session
tools/xtrace-env session list

# 诊断指定 Session
tools/xtrace-env session doctor -s 1
tools/xtrace-env session doctor -s 1 -json

# 关闭指定 Session
tools/xtrace-env session kill 1

# 关闭所有 Session
tools/xtrace-env session kill all

# 关闭最新 Session
tools/xtrace-env close
```

### 信号追踪

```bash
# 追踪驱动
tools/xtrace-env driver test_top.uut.bus.ready -s 1
tools/xtrace-env driver test_top.uut.bus.ready -s 1 -json

# 追踪负载
tools/xtrace-env load test_top.valid_in -s 1
tools/xtrace-env load test_top.valid_in -s 1 -json

# 过滤 trace 结果
tools/xtrace-env driver test_top.uut.bus.ready -s 1 -json --limit 5
tools/xtrace-env load test_top.valid_in -s 1 -json --role rhs_use --no-statement-only
```

`driver/load -json` 使用同一套 trace engine 渲染，典型输出字段如下：

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

### AI Agent 推荐入口

```bash
# 一条命令完成 session ensure + driver trace
tools/xtrace-env query -dbdir /path/to/simv.daidir --driver test_top.uut.bus.ready -json --limit 10

# 一条命令完成 session ensure + load trace
tools/xtrace-env query -dbdir /path/to/simv.daidir --load test_top.valid_in -json --role rhs_use
```

`query -json` 输出包含 `session` 和 `trace` 两段；失败时输出 `ok=false`、`status` 和 `message`，便于 agent 做下一步决策。

### 信号发现

```bash
# 解析完整信号名
tools/xtrace-env signal resolve test_top.uut.bus.ready -s 1 -json

# 按短名或片段搜索候选信号
tools/xtrace-env signal search ready -s 1 -json --limit 20
```

JSON 输出包含 `ok/query/matches/count/truncated`；找不到信号时返回非 0，并给出 `status=not_found`。

### Session 诊断退出码

`session doctor` 是脚本化健康检查入口：

- 健康 Session 返回 `0`，状态为 `healthy`
- 不健康 Session 返回非 `0`
- 缺少 `-s <sid>` 或非法 Session ID 返回非 `0`
- JSON 输出包含 `session_id`、`healthy`、`status`、`message`、`pid`、`socket_path`、`design_file`、`dbdir_path` 和 daidir metadata

状态值：

- `dbdir_missing` — daidir 路径不存在或不是目录
- `dbdir_changed` — daidir 的 mtime/size/dev/inode 与 open 时记录不一致
- `registry_missing` — registry 中没有该 Session
- `process_exited` — server 进程不存在
- `socket_missing` — socket 文件缺失
- `connect_failed` — socket 存在但无法连接
- `ping_failed` — server 未响应 `PING`
- `healthy` — registry、进程、socket、连接和 `PING/PONG` 均正常

## 项目结构

```
xtrace/
├── src/
│   ├── main.cpp                    # CLI 入口与命令路由
│   ├── commands/                   # 命令实现
│   │   ├── cmd_session.cpp         # Session 管理命令
│   │   └── cmd_trace.cpp           # Driver/Load 追踪命令
│   ├── session/                    # Session 管理核心
│   │   ├── session_manager.cpp
│   │   └── session_registry.cpp
│   ├── client/                     # 客户端通信
│   │   └── client.cpp
│   ├── server/                     # NPI Server 主逻辑
│   │   └── server.cpp
│   ├── trace/                      # Driver/Load trace engine
│   │   ├── trace_engine.cpp
│   │   └── trace_engine.h
│   ├── signal/                     # Signal resolve/search
│   │   ├── signal_finder.cpp
│   │   └── signal_finder.h
│   ├── control_dep/                # 控制依赖分析
│   │   ├── control_dep.cpp
│   │   └── control_dep.h
│   └── protocol/
│       └── protocol.h              # 通信协议定义
├── tools/
│   └── xtrace-env                  # Runtime wrapper for VERDI_HOME/LD_LIBRARY_PATH
├── skill/
│   └── SKILL.md                    # Agent skill for xtrace workflows
├── Makefile
└── README.md
```

## 依赖

- Synopsys Verdi `V-2023.12-SP2+`
- NPI 库 (`libNPI.so`, `libnpiL1.so`)
- C++11 编译器，需支持 libstdc++ new C++11 ABI (`std::__cxx11::basic_string`)

### 已验证工具版本

- Verdi: `V-2023.12-SP2` for `linux64`
- VCS: `V-2023.12-SP2_Full64`
- G++: GCC `8.5.0`

说明：

- 使用 VCS 编译测试 daidir 时，本机需要设置 `VCS_TARGET_ARCH=linux64`。
- NPI L1 的 FSDB/RTL C++ API 使用 `std::string`，需要和 Synopsys `libnpiL1.so`
  的 C++ ABI 一致。若链接时报 `std::string` / `std::__cxx11::basic_string`
  相关 undefined reference，请使用 GCC 5+，并避免
  `-D_GLIBCXX_USE_CXX11_ABI=0`。

## 已知限制

- 当前基于 **SV Language Model**（RTL 级），对于某些 NPI 本身不支持的过程赋值场景，driver/load 可能返回空结果
- 通用名称追踪模式暂不做模块端口引用路径映射，某些深层 interface 驱动源可能需要提供模块端口引用路径才能追踪
