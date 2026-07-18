# Agent HTTP Server 生命周期与配置

## 生命周期

HTTP Server 使用显式四阶段生命周期：

```text
Stopped → Starting → Running → Stopping → Stopped
```

- `Start()` 同步校验配置、绑定监听地址并启动 server worker，成功后返回实际端口。
- `RequestStop()` 可由任意线程重复调用，不抛异常。
- `Wait()` 等待 worker 退出，关闭 acceptor，释放端口并传播 worker 异常。
- `Run(stop_requested)` 是兼容入口，内部组合 `Start()`、停止条件轮询、`RequestStop()` 和 `Wait()`。
- 析构函数会请求停止并回收 worker，不允许线程脱离对象生命周期。
- 完整停止后，同一个 `HttpServer` 实例可以再次启动。

端口配置为 `0` 时，操作系统会选择空闲端口。`Start()` 的返回值和 `BoundPort()` 可用于获取实际端口；完成 `Wait()` 后，`BoundPort()` 恢复为 `0`。

## 配置

| 配置项 | 默认值 | 作用 |
| --- | ---: | --- |
| `bind_address` | `127.0.0.1` | Agent 监听地址 |
| `port` | `18081` | 监听端口，`0` 表示随机端口 |
| `io_thread_count` | `1` | 后续异步 I/O worker 数量 |
| `handler_thread_count` | `4` | 后续业务处理 worker 数量 |
| `max_connections` | `128` | 后续连接上限；当前作为 listen backlog 上限 |
| `max_in_flight_requests` | `64` | 后续业务队列的在途请求上限 |
| `max_header_bytes` | `16 KiB` | 后续 Session 的 Header 上限 |
| `max_body_bytes` | `1 MiB` | 后续 Session 的 Body 上限 |
| `max_requests_per_connection` | `100` | 后续 Keep-Alive 单连接请求上限 |
| `read_timeout` | `5s` | 当前同步读取和后续异步读取超时 |
| `write_timeout` | `15s` | 当前同步写入和后续异步写入超时 |
| `idle_timeout` | `30s` | 后续 Keep-Alive 空闲超时 |

本阶段已经启用监听 backlog、读取超时和写入超时。线程数量、在途请求和 HTTP 协议限制已经统一配置并在启动时校验，将在异步 Session 和有界业务线程池阶段正式执行。

## 失败行为

- 地址格式错误、端口占用、监听失败和线程创建失败由 `Start()` 立即抛出。
- 配置中的线程数、容量、协议限制或超时为零时，`Start()` 抛出 `std::invalid_argument`。
- 重复调用 `Start()` 或在尚未完成停止时启动，会抛出 `std::logic_error`。
- `RequestStop()` 幂等；重复停止不会产生错误。
- worker 内部异常由 `Wait()` 传播；析构路径会吞掉异常但仍完成资源回收。

## 当前边界

生命周期重构没有改变现有 HTTP API 和 JSON 格式。当前 Session 仍采用同步单请求处理；异步 Accept、并发 Session、有界业务线程池和 Keep-Alive 分别由后续步骤实现。
