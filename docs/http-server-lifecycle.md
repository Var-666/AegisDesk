# Agent HTTP Server 生命周期与配置

## 生命周期

HTTP Server 使用显式四阶段生命周期：

```text
Stopped → Starting → Running → Stopping → Stopped
```

- `Start()` 同步校验配置、绑定监听地址并启动 I/O workers，成功后返回实际端口。
- `RequestStop()` 可由任意线程重复调用，不抛异常。
- `Wait()` 等待全部 I/O worker 退出，关闭 acceptor，释放端口并传播 worker 异常。
- `Run(stop_requested)` 是兼容入口，内部组合 `Start()`、停止条件轮询、`RequestStop()` 和 `Wait()`。
- 析构函数会请求停止并回收全部 worker 和 Session，不允许线程脱离对象生命周期。
- 完整停止后，同一个 `HttpServer` 实例可以再次启动。

端口配置为 `0` 时，操作系统会选择空闲端口。`Start()` 的返回值和 `BoundPort()` 可用于获取实际端口；完成 `Wait()` 后，`BoundPort()` 恢复为 `0`。

## 配置

| 配置项 | 默认值 | 作用 |
| --- | ---: | --- |
| `bind_address` | `127.0.0.1` | Agent 监听地址 |
| `port` | `18081` | 监听端口，`0` 表示随机端口 |
| `io_thread_count` | `1` | 当前异步 I/O worker 数量 |
| `handler_thread_count` | `4` | 后续业务处理 worker 数量 |
| `max_connections` | `128` | 活跃 Session 上限及 listen backlog 上限 |
| `max_in_flight_requests` | `64` | 后续业务队列的在途请求上限 |
| `max_header_bytes` | `16 KiB` | 后续 Session 的 Header 上限 |
| `max_body_bytes` | `1 MiB` | 后续 Session 的 Body 上限 |
| `max_requests_per_connection` | `100` | 后续 Keep-Alive 单连接请求上限 |
| `read_timeout` | `5s` | 异步读取超时 |
| `write_timeout` | `15s` | 异步写入超时 |
| `idle_timeout` | `30s` | 后续 Keep-Alive 空闲超时 |

当前已经启用异步 I/O worker、活跃 Session 限制、监听 backlog、读取超时和写入超时。在途请求和 HTTP 协议限制已经统一配置并在启动时校验，将在有界业务线程池与协议加固阶段正式执行。

## 异步连接模型

```text
async_accept
    ↓
独立 HttpSession + 独立 strand
    ↓
async_read → AgentApi handler → async_write
    ↓
关闭并从活跃 Session 集合移除
```

- Listener strand 串行化 accept、cancel 和 close，避免多个 I/O worker 竞争 acceptor。
- 每个 socket 使用独立 strand，保证同一 Session 的读取、写入、超时和关闭回调不会并发执行。
- Session 由 `shared_ptr` 管理异步生命周期，Server 持有活跃 Session 集合。
- 停止时先关闭 Listener，再取消所有活跃 Session，最后释放 work guard 并回收 I/O workers。
- 空连接只占用自己的异步读取，不会阻塞 Listener 接受和处理其他连接。

## 失败行为

- 地址格式错误、端口占用、监听失败和线程创建失败由 `Start()` 立即抛出。
- 配置中的线程数、容量、协议限制或超时为零时，`Start()` 抛出 `std::invalid_argument`。
- 重复调用 `Start()` 或在尚未完成停止时启动，会抛出 `std::logic_error`。
- `RequestStop()` 幂等；重复停止不会产生错误。
- I/O worker 内部异常由 `Wait()` 传播；析构路径会吞掉异常但仍完成资源回收。

## 当前边界

异步改造没有改变现有 HTTP API 和 JSON 格式。当前每个 Session 仍只处理一个请求，且同步 `AgentApi` handler 暂时运行在 I/O worker 上；有界业务线程池、背压和 Keep-Alive 由后续步骤实现。
