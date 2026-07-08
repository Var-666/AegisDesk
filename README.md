# AegisDesk

AegisDesk is a local desktop console for managing C++ service processes. It provides a Qt UI backed by a local agent that can register services from config, start/stop/restart them, read recent logs, and expose runtime metrics.

## Current Capabilities

- Config-driven multi-service registry via `configs/services.json`
- Local HTTP JSON control plane on `127.0.0.1`
- Process lifecycle management: start, stop, restart, status
- Per-service log tailing
- Runtime metrics: CPU, RSS memory, thread count, open FD count
- Metrics history endpoint and Qt Charts trend view
- Qt desktop UI with service list, overview tab, logs, metrics cards, and trends tab
- `auto_start` support for services started with the agent

## Project Layout

```text
AegisDesk/
├── CMakeLists.txt
├── apps/
│   ├── agent/          # Local HTTP agent, process supervision, metrics
│   ├── desktop/        # Qt desktop client
│   └── demo_service/   # Demo managed service
├── configs/
│   └── services.json
└── runtime/
    └── logs/
```

## Requirements

- CMake 3.30+
- C++20 compiler
- Boost 1.74+
- Qt 6 components:
  - Core
  - Gui
  - Widgets
  - Network
  - Charts

On macOS with Homebrew:

```bash
brew install cmake boost qt qtcharts
```

On Ubuntu/Debian, install the equivalent Qt 6 base and charts development packages for your distribution.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Main outputs:

```text
build/apps/agent/agent
build/apps/desktop/desktop
build/apps/demo_service/demo_service
```

Note: the top-level `CMakeLists.txt` currently sets a Homebrew Boost path. On non-Homebrew systems, remove that line or pass the proper Boost location through CMake.

## Run

Start the agent from the project root:

```bash
./build/apps/agent/agent \
  --config configs/services.json \
  --work-dir . \
  --port 18081
```

Start the desktop app:

```bash
./build/apps/desktop/desktop \
  --agent-url http://127.0.0.1:18081
```

## Service Config

Services are defined in `configs/services.json`:

```json
{
  "schema_version": 1,
  "services": [
    {
      "id": "demo_service",
      "display_name": "Demo Service",
      "executable": "build/apps/demo_service/demo_service",
      "work_dir": ".",
      "args": ["--name", "demo_service", "--interval-ms", "1000"],
      "log_path": "runtime/logs/demo_service.log",
      "auto_start": false
    }
  ]
}
```

Service IDs may contain only letters, digits, `_`, and `-`.

## Agent API

```http
GET  /api/v1/services
GET  /api/v1/services/{service_id}/status
GET  /api/v1/services/{service_id}/logs?tail=100
GET  /api/v1/services/{service_id}/metrics
GET  /api/v1/services/{service_id}/metrics/history?limit=300
POST /api/v1/services/{service_id}/start
POST /api/v1/services/{service_id}/stop
POST /api/v1/services/{service_id}/restart
```

Example:

```bash
curl -s http://127.0.0.1:18081/api/v1/services
curl -s -X POST http://127.0.0.1:18081/api/v1/services/demo_service/start
curl -s http://127.0.0.1:18081/api/v1/services/demo_service/metrics
```

## Metrics

The agent uses a platform metrics reader abstraction:

- Linux: `/proc`-based reader
- macOS: `libproc`/Mach-based reader
- Other platforms: unsupported reader

The desktop shows current metrics in the Overview tab and historical charts in the Trends tab.

## Known Limitations

- Process supervision is POSIX-oriented; Windows process management is not implemented.
- Agent HTTP handling is synchronous and single-connection oriented.
- Service config is loaded at agent startup; hot reload is not implemented.
- No authentication, TLS, audit log, or permission model yet.
- No automated tests are currently included.
- Metrics are stored in memory only; there is no persistent history database.
