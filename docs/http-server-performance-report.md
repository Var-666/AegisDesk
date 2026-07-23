# AegisDesk HTTP Server 性能验收报告

> 自动生成于 2026-07-23 06:15:11 UTC。每个场景执行 3 次，表格记录吞吐量居中的一次结果。

## 测试环境

| 项目 | 值 |
| --- | --- |
| 操作系统 | macOS |
| 架构 | arm64 |
| 处理器 | Apple M1 |
| 编译器 | Clang 21.0 |
| 构建类型 | Release |
| 硬件并发数 | 8 |

## 验收结果

| 场景 | 并发连接 | 请求数 | 成功率 | 吞吐量（req/s） | P50 | P95 | P99 | 最大并行 Handler | 状态 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| short-connections | 32 | 800 | 100.00% | 30,039.19 | 1.02ms | 1.23ms | 1.79ms | 1 | 通过 |
| keep-alive | 32 | 3,200 | 100.00% | 108,563.82 | 0.29ms | 0.36ms | 0.41ms | 2 | 通过 |
| handler-pool-5ms | 16 | 160 | 100.00% | 655.23 | 23.87ms | 25.06ms | 25.21ms | 4 | 通过 |

## 场景说明

- `short-connections`：32 个客户端各创建 25 次短连接，包含 TCP 建连、请求和响应开销。
- `keep-alive`：32 个客户端各复用一个连接完成 100 次请求，验证持续吞吐和连接复用。
- `handler-pool-5ms`：16 个客户端持续请求，Handler 每次模拟 5ms 业务工作，验证有界线程池并行能力。

## 验收口径

- 所有请求必须返回预期的 HTTP 200 JSON 响应。
- Server 停止后，活跃 Session 和在途请求计数必须归零。
- 延迟和吞吐量用于同一环境下的版本趋势对比，不作为跨机器的固定门槛。
- CI 中的 `performance` 测试另行检查 50 并发、Keep-Alive 持续负载和 Handler 并行加速。

## 复现方法

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release \
  --target aegis_http_performance_tests aegis_http_benchmark \
  --parallel
ctest --test-dir build-release -L performance --output-on-failure
./build-release/tests/aegis_http_benchmark \
  --output http-performance-report.md \
  --repetitions 3
```

报告使用本机回环 TCP 和轻量 JSON Handler，主要用于验证 Agent HTTP Server 自身的并发模型。实际 API 性能还会受到业务数据量、进程状态查询和日志读取成本影响。
