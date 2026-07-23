# Agent HTTP Server 生命周期与配置

## 生命周期

HTTP Server 使用显式四阶段生命周期：

```text
Stopped → Starting → Running → Stopping → Stopped
```

- `Start()` 同步校验配置、绑定监听地址并启动 I/O workers 与业务 Handler workers，成功后返回实际端口。
- `RequestStop()` 可由任意线程重复调用，不抛异常；它停止接受新连接和新业务任务，并启动优雅关闭。
- `Wait()` 排空已接受的业务任务和响应，等待全部 Handler 与 I/O worker 退出，关闭 acceptor，释放端口并传播 worker 异常。
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
| `handler_thread_count` | `4` | 独立业务处理 worker 数量 |
| `max_connections` | `128` | 活跃 Session 上限及 listen backlog 上限 |
| `max_in_flight_requests` | `64` | 正在执行与排队请求的总上限 |
| `max_header_bytes` | `16 KiB` | 单请求 Header 上限 |
| `max_body_bytes` | `1 MiB` | 单请求 Body 上限 |
| `max_requests_per_connection` | `100` | Keep-Alive 单连接请求上限 |
| `read_timeout` | `5s` | 异步读取超时 |
| `write_timeout` | `15s` | 异步写入超时 |
| `idle_timeout` | `30s` | Keep-Alive 等待下一请求的空闲超时 |
| `shutdown_grace_period` | `5s` | 停机时等待活跃连接完成响应的宽限期 |

所有配置均在 `Start()` 时校验。每个请求使用独立 Beast Parser，Header 和 Body 在解析阶段受限；Keep-Alive 连接达到请求数或空闲时间上限后主动关闭。

## 异步连接模型

```text
async_accept
    ↓
独立 HttpSession + 独立 strand
    ↓
受限 Parser + async_read → 有界业务执行器 → AgentApi handler
              ↓ 饱和
         503 + Retry-After
    ↓ Handler 结果回投 Session strand
async_write
    ↓ Keep-Alive 且未达到上限
重新创建 Parser 并读取下一请求
    ↓ Connection: close / 超时 / 上限
关闭并从活跃 Session 集合移除
```

- Listener strand 串行化 accept、cancel 和 close，避免多个 I/O worker 竞争 acceptor。
- 每个 socket 使用独立 strand，保证同一 Session 的读取、写入、超时和关闭回调不会并发执行。
- Session 由 `shared_ptr` 管理异步生命周期，Server 持有活跃 Session 集合。
- 业务 Handler 只在独立执行器中运行，不占用 I/O worker；完成后将响应回投对应 Session strand。
- `max_in_flight_requests` 同时限制正在运行和排队的任务，容量耗尽时不继续排队，而是立即返回 `503 Service Unavailable` 和 `Retry-After: 1`。
- HTTP/1.0 与 HTTP/1.1 的连接复用由请求语义决定；响应版本与请求一致，达到 `max_requests_per_connection` 后响应声明关闭连接。
- 首次请求使用 `read_timeout`，连接复用期间等待下一请求使用 `idle_timeout`，写响应使用 `write_timeout`。
- 空连接只占用自己的异步读取，不会阻塞 Listener 接受和处理其他连接。

## 优雅关闭

```text
停止 accept 与新业务提交
    ↓
关闭空闲/仍在读取的连接
    ↓
排空已经提交的排队与运行任务
    ↓
写完响应并声明 Connection: close
    ↓ 超过 shutdown_grace_period
强制关闭剩余客户端连接
    ↓
回收 Handler workers、Session 和 I/O workers
```

宽限期限制的是客户端连接存活时间。C++ 无法安全强制终止正在执行的任意 Handler，因此 Handler 超过宽限期时连接会被强制关闭，但 `Wait()` 仍会等待该 Handler 返回，避免后台线程访问已经析构的业务对象。

## 失败行为

- 地址格式错误、端口占用、监听失败和线程创建失败由 `Start()` 立即抛出。
- 配置中的线程数、容量、协议限制或超时为零时，`Start()` 抛出 `std::invalid_argument`。
- 重复调用 `Start()` 或在尚未完成停止时启动，会抛出 `std::logic_error`。
- `RequestStop()` 幂等；重复停止不会产生错误。
- Header 超限返回 `431 Request Header Fields Too Large`，Body 超限返回 `413 Payload Too Large`，可识别的格式错误返回 `400 Bad Request`，这些请求不会进入业务执行器。
- 业务执行器达到容量上限时返回 JSON 错误 `server_overloaded`，HTTP 状态为 `503`，不会阻塞 I/O 线程等待队列空间。
- I/O worker 内部异常由 `Wait()` 传播；析构路径会吞掉异常但仍完成资源回收。

## 韧性测试

`aegis_http_server_tests` 中的 `resilience` 测试组覆盖并发 Keep-Alive、并发停止、半包超时、客户端中途断开、连接容量耗尽与恢复，以及 50 轮生命周期文件描述符回收。该测试组可以独立执行：

```bash
ctest --test-dir build -L resilience --output-on-failure
```

## 当前边界

HTTP 协议层现在支持受限 Keep-Alive、解析限制和有界优雅关闭。Agent 仍定位为本机控制面；TLS、身份认证、权限模型和审计日志不属于当前 HTTP Server 范围。
