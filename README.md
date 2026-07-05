# AegisDesk

> 面向本地 C++ 服务的轻量级桌面治理与运行控制平台。
> 当前版本已实现：**配置驱动的多服务注册、进程生命周期管理、HTTP 控制面、Qt 服务列表、实时日志查看与服务启停控制。**

AegisDesk 的目标不是做一个简单的“启动器”，而是逐步构建一个具备服务治理、可观测性、故障诊断和运行控制能力的 C++ / Qt 工具平台。

当前 MVP 已经形成完整闭环：

```text
Qt Desktop
    │
    │ HTTP + JSON
    ▼
Local Agent
    │
    ├── ServiceRegistry
    │   ├── demo_service
    │   └── demo_worker
    │
    ├── ProcessSupervisor
    │   ├── fork
    │   ├── execv
    │   ├── SIGTERM
    │   ├── SIGKILL
    │   └── waitpid
    │
    └── LogReader
            │
            ▼
      demo_service instances
```

---

## 1. 当前能力

| 模块       | 已实现能力                                          |
|----------|------------------------------------------------|
| 多服务注册    | 通过 `configs/services.json` 配置服务实例              |
| 服务生命周期   | 启动、停止、重启、状态查询                                  |
| 多实例运行    | 同一个 `demo_service` 可通过不同参数运行多个实例               |
| 进程管理     | `fork + execv` 启动，`SIGTERM` 优雅停止，超时后 `SIGKILL` |
| 进程回收     | 使用 `waitpid` 回收子进程，避免僵尸进程                      |
| 服务状态     | PID、运行状态、运行时长、最近退出码                            |
| 日志管理     | 每个服务独立日志文件，支持读取尾部日志                            |
| HTTP 控制面 | Agent 提供 REST 风格 JSON API                      |
| Qt 桌面端   | 左侧多服务列表，右侧服务详情、日志与控制按钮                         |
| 自动启动     | 支持通过 `auto_start` 配置 Agent 启动时自动拉起服务           |
| 本地安全边界   | Agent 默认仅监听 `127.0.0.1`                        |

---

## 2. 项目结构

```text
AegisDesk/
├── CMakeLists.txt
├── README.md
│
├── apps/
│   ├── demo_service/
│   │   ├── CMakeLists.txt
│   │   └── main.cpp
│   │
│   ├── agent/
│   │   ├── CMakeLists.txt
│   │   ├── include/agent/
│   │   │   ├── agent_api.h
│   │   │   ├── http_json.h
│   │   │   ├── http_server.h
│   │   │   ├── log_reader.h
│   │   │   ├── process_supervisor.h
│   │   │   ├── service_definition.h
│   │   │   └── service_registry.h
│   │   └── src/
│   │       ├── agent_api.cpp
│   │       ├── http_json.cpp
│   │       ├── http_server.cpp
│   │       ├── log_reader.cpp
│   │       ├── main.cpp
│   │       ├── process_supervisor.cpp
│   │       └── service_registry.cpp
│   │
│   └── desktop/
│       ├── CMakeLists.txt
│       ├── include/desktop/
│       │   ├── agent_client.h
│       │   └── main_window.h
│       └── src/
│           ├── agent_client.cpp
│           ├── main.cpp
│           └── main_window.cpp
│
├── configs/
│   └── services.json
│
├── runtime/
│   └── logs/
│
└── build/
```

---

## 3. 核心设计

### 3.1 配置驱动服务注册

AegisDesk 不将服务列表写死在 Agent 代码中。

服务由 `configs/services.json` 定义：

```text
配置文件
    ↓
ServiceDefinition
    ↓
ServiceRegistry
    ↓
多个 ProcessSupervisor
    ↓
多个被管理服务进程
```

新增服务时，通常只需要增加一段 JSON 配置，不需要修改 Agent 的核心生命周期代码。

---

### 3.2 多服务模型

每一个服务对应一个独立的 `ProcessSupervisor`：

```text
ServiceRegistry
    ├── demo_service → ProcessSupervisor
    ├── demo_worker  → ProcessSupervisor
    └── future_service → ProcessSupervisor
```

每个服务拥有独立的：

* 服务 ID
* 展示名称
* 可执行文件
* 工作目录
* 启动参数
* 日志文件
* 自动启动策略
* PID 与运行状态
* 退出码与运行时长

---

### 3.3 生命周期管理

服务状态流转：

```text
Stopped
   │
   │ Start
   ▼
Running
   │
   │ Stop
   ▼
Stopping
   │
   ├── SIGTERM 后正常退出
   │
   └── 超时后 SIGKILL
   ▼
Stopped
```

停止逻辑：

1. Agent 向子进程发送 `SIGTERM`。
2. 等待最多 3 秒，让服务优雅退出。
3. 超时后发送 `SIGKILL`。
4. 使用 `waitpid` 回收子进程。
5. 保存最近一次退出码。

---

### 3.4 Qt 客户端分层

Qt Desktop 不直接操作 Linux 进程。

```text
MainWindow
    ├── 服务列表
    ├── 当前服务详情
    ├── 日志展示
    ├── 控制按钮
    └── 定时刷新

AgentClient
    ├── HTTP 请求
    ├── JSON 解析
    ├── 错误转换
    └── ServiceSnapshot 数据模型

Agent
    ├── REST API
    ├── ServiceRegistry
    ├── ProcessSupervisor
    └── LogReader
```

这种分层使后续加入指标、图表、告警、配置修改、服务拓扑时，不需要让 Qt 页面直接依赖底层进程控制细节。

---

## 4. 环境要求

当前版本面向 Linux / POSIX 环境。

### 编译依赖

* CMake >= 3.24
* 支持 C++20 的编译器
* Boost >= 1.74
* Qt 6

    * Core
    * Gui
    * Widgets
    * Network

Ubuntu / Debian 示例：

```bash
sudo apt update
sudo apt install -y \
  cmake \
  g++ \
  make \
  libboost-dev \
  qt6-base-dev
```

---

## 5. 构建

在项目根目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j "$(nproc)"
```

构建完成后，主要可执行文件位于：

```text
build/apps/demo_service/demo_service
build/apps/agent/agent
build/apps/desktop/aegis_desktop
```

---

## 6. 服务配置

默认配置文件：

```text
configs/services.json
```

示例：

```json
{
  "schema_version": 1,
  "services": [
    {
      "id": "demo_service",
      "display_name": "Demo Service",
      "executable": "build/apps/demo_service/demo_service",
      "work_dir": ".",
      "args": [
        "--name",
        "demo_service",
        "--interval-ms",
        "1000"
      ],
      "log_path": "runtime/logs/demo_service.log",
      "auto_start": false
    },
    {
      "id": "demo_worker",
      "display_name": "Demo Worker",
      "executable": "build/apps/demo_service/demo_service",
      "work_dir": ".",
      "args": [
        "--name",
        "demo_worker",
        "--interval-ms",
        "1500"
      ],
      "log_path": "runtime/logs/demo_worker.log",
      "auto_start": false
    }
  ]
}
```

### 字段说明

| 字段               | 说明                 |
|------------------|--------------------|
| `schema_version` | 配置结构版本，当前为 `1`     |
| `id`             | 服务唯一标识，用于 API 路由   |
| `display_name`   | Qt 中显示的名称          |
| `executable`     | 服务可执行文件路径          |
| `work_dir`       | 服务运行时的工作目录         |
| `args`           | 传递给服务进程的命令行参数      |
| `log_path`       | 服务日志路径             |
| `auto_start`     | Agent 启动时是否自动拉起该服务 |

### 服务 ID 约束

`id` 只能包含：

```text
英文字母、数字、下划线 `_`、短横线 `-`
```

例如：

```text
gateway
demo_service
payment-worker
worker_01
```

---

## 7. 启动

### 7.1 启动 Agent

在项目根目录执行：

```bash
./build/apps/agent/agent \
  --config configs/services.json \
  --work-dir . \
  --port 18081
```

预期输出：

```text
AegisDesk Agent loaded 2 service definitions
  - demo_service (Demo Service), auto_start=false
  - demo_worker (Demo Worker), auto_start=false
AegisDesk Agent listening on http://127.0.0.1:18081
```

Agent 默认监听：

```text
http://127.0.0.1:18081
```

当前版本没有鉴权和 TLS，因此不应直接绑定到公网地址或局域网地址。

---

### 7.2 启动 Qt Desktop

另开一个终端：

```bash
./build/apps/desktop/aegis_desktop \
  --agent-url http://127.0.0.1:18081
```

Qt 界面提供：

* 服务列表
* 当前服务详情
* 状态、PID、运行时间、退出码
* 启动、停止、重启
* 最近日志查看
* 自动轮询刷新

---

## 8. Demo Service 参数

`demo_service` 支持多实例运行：

```bash
./build/apps/demo_service/demo_service \
  --name demo_worker \
  --interval-ms 1500
```

参数说明：

| 参数              | 作用           |
|-----------------|--------------|
| `--name`        | 服务实例名称       |
| `--interval-ms` | 心跳日志间隔，单位为毫秒 |

默认行为：

```bash
./build/apps/demo_service/demo_service
```

等价于：

```bash
./build/apps/demo_service/demo_service \
  --name demo_service \
  --interval-ms 1000
```

不同实例会写入独立日志：

```text
runtime/logs/demo_service.log
runtime/logs/demo_worker.log
```

---

## 9. Agent API

### 查询全部服务

```http
GET /api/v1/services
```

示例：

```bash
curl -s http://127.0.0.1:18081/api/v1/services
```

响应示例：

```json
{
  "services": [
    {
      "id": "demo_service",
      "display_name": "Demo Service",
      "auto_start": false,
      "state": "stopped",
      "pid": -1,
      "uptime_seconds": 0,
      "last_exit_code": null
    }
  ]
}
```

---

### 查询单个服务状态

```http
GET /api/v1/services/{service_id}/status
```

示例：

```bash
curl -s \
  http://127.0.0.1:18081/api/v1/services/demo_service/status
```

---

### 启动服务

```http
POST /api/v1/services/{service_id}/start
```

示例：

```bash
curl -s -X POST \
  http://127.0.0.1:18081/api/v1/services/demo_service/start
```

---

### 停止服务

```http
POST /api/v1/services/{service_id}/stop
```

示例：

```bash
curl -s -X POST \
  http://127.0.0.1:18081/api/v1/services/demo_service/stop
```

---

### 重启服务

```http
POST /api/v1/services/{service_id}/restart
```

示例：

```bash
curl -s -X POST \
  http://127.0.0.1:18081/api/v1/services/demo_service/restart
```

---

### 查看服务日志

```http
GET /api/v1/services/{service_id}/logs?tail=100
```

示例：

```bash
curl -s \
  "http://127.0.0.1:18081/api/v1/services/demo_worker/logs?tail=50"
```

---

## 10. 当前功能状态

| 功能             | 状态  |
|----------------|-----|
| 配置驱动服务注册       | 已完成 |
| 多服务实例          | 已完成 |
| 服务启停重启         | 已完成 |
| 独立日志文件         | 已完成 |
| Agent HTTP API | 已完成 |
| Qt 多服务列表       | 已完成 |
| Qt 服务详情页       | 已完成 |
| 日志尾部查询         | 已完成 |
| 自动启动           | 已完成 |
| CPU / 内存采集     | 未完成 |
| 线程数 / FD 数采集   | 未完成 |
| 指标趋势图          | 未完成 |
| WebSocket 推送   | 未完成 |
| 自动拉起策略         | 未完成 |
| 告警规则           | 未完成 |
| SQLite 历史记录    | 未完成 |
| 服务调用拓扑         | 未完成 |
| 故障注入           | 未完成 |
| 远程 Agent       | 未完成 |
| 鉴权与 TLS        | 未完成 |
| Windows 进程管理实现 | 未完成 |

---

## 11. 已知限制

当前版本属于 MVP，存在以下明确限制：

* 仅支持 Linux / POSIX 进程管理接口。
* Agent 默认单线程串行处理 HTTP 请求。
* 日志读取采用尾部行读取，超大日志文件后续需要优化。
* 服务配置只在 Agent 启动时加载，不支持热更新。
* 当前没有权限模型、鉴权、TLS 或审计机制。
* Qt 在服务控制请求进行期间会临时禁用控制按钮，避免并发提交冲突的生命周期操作。
* Agent 退出时会按服务注册顺序的逆序停止已启动服务。

---

## 12. 后续路线图

### Phase 1：运行指标

* CPU 使用率
* 内存占用
* 线程数
* 文件描述符数量
* 服务启动次数
* 服务异常退出次数

### Phase 2：可视化能力

* 指标卡片
* CPU / 内存趋势图
* 错误日志聚合
* 服务状态颜色标识
* 服务筛选与搜索

### Phase 3：稳定性治理

* 自动重启策略
* 最大重启次数
* 退避重启
* 健康检查
* 告警规则
* 告警历史

### Phase 4：服务关系与故障演练

* 服务调用拓扑图
* 服务依赖关系
* 模拟延迟
* 模拟服务崩溃
* 模拟日志异常
* 故障恢复报告

---

## 13. 项目目标

AegisDesk 希望最终形成以下能力：

```text
服务注册
    ↓
生命周期管理
    ↓
运行状态采集
    ↓
日志与指标分析
    ↓
告警与自动恢复
    ↓
拓扑与故障演练
    ↓
可观测性与服务治理平台
```

当前版本已经完成了这个目标中的第一条核心链路：

> **通过配置注册多个服务，由 Agent 管理其生命周期，并由 Qt Desktop 提供可视化控制与日志观察能力。**
