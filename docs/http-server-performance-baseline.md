# Agent HTTP Server 性能基线

## 目的

本文记录 HTTP Server 并发优化前的性能表现，用于后续异步 I/O 和业务线程池改造的对照。基线测试通过真实 TCP 连接访问绑定到随机端口的 Agent HTTP Server，不使用 Mock 网络层。

初始基线版本采用单线程同步处理模型：

```text
accept → read → AgentApi handler → write → 下一连接
```

因此，基线版本的总耗时和 P95 延迟会随并发客户端数量近似线性增长。

## 测试方法

- 构建类型：Debug
- 客户端数量：1、10、20、50
- 每个客户端发送一个 HTTP GET 请求
- 所有客户端通过启动门同步发起请求
- Handler 固定模拟 10ms 业务处理时间
- 记录成功率、批次总耗时和 P95 请求延迟
- 测试实现：`tests/integration/http_server_baseline_test.cpp`

运行命令：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build --target aegis_http_server_tests --parallel
ctest --test-dir build -L baseline --verbose
```

## 初始基线

记录日期：2026-07-17

测试环境：Apple M1、arm64、macOS 26.5.1、Debug 构建。

| 并发客户端 | 成功率 | 总耗时 | P95 延迟 |
| ---: | ---: | ---: | ---: |
| 1 | 100% | 64.45ms | 64.42ms |
| 10 | 100% | 177.74ms | 177.56ms |
| 20 | 100% | 299.00ms | 286.91ms |
| 50 | 100% | 665.00ms | 639.52ms |

这些数据用于确认趋势，不作为跨机器的固定性能断言。CTest 只要求所有并发请求成功，实际延迟会输出到测试日志，避免因 CI 主机性能波动造成误报。

## 异步 Listener 与 Session 阶段

记录日期：2026-07-18

完成 `async_accept`、独立 Session strand、`async_read`、`async_write` 和 4 个 I/O workers 后，在每个 handler 模拟 100ms 业务工作的场景中：

| 并发客户端 | 成功率 | 总耗时 | P95 延迟 | 最大并行 Handler |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 100% | 约 220ms | 约 220ms | 4 |

串行执行 8 个请求理论上至少需要约 800ms。该测试同时验证：

- 单个空闲客户端不会阻塞其他完整请求。
- 多个 I/O worker 能处理独立 Session。
- 停止时能够主动取消仍在等待读取的 Session。

业务 handler 当前仍占用 I/O worker，因此这只是网络 I/O 异步化结果，不能替代下一阶段的有界业务线程池。

## 业务隔离与背压阶段

记录日期：2026-07-22

完成独立有界业务执行器后，默认配置使用 1 个 I/O worker、4 个 Handler workers 和最多 64 个在途请求。相同的 10ms 模拟业务测试结果为：

| 并发客户端 | 成功率 | 总耗时 | P95 延迟 |
| ---: | ---: | ---: | ---: |
| 1 | 100% | 13.28ms | 13.25ms |
| 10 | 100% | 35.88ms | 35.75ms |
| 20 | 100% | 63.74ms | 63.32ms |
| 50 | 100% | 157.08ms | 144.29ms |

在 8 个并发请求、每个 Handler 模拟 100ms 工作的隔离测试中，即使只有 1 个 I/O worker，4 个 Handler workers 仍能达到 4 路并行，总耗时稳定在约 206–214ms。新增测试同时验证：

- Handler 执行不会占用唯一的 I/O worker。
- “正在执行 + 排队”的请求数达到上限时，新请求快速返回 `503 Service Unavailable`。
- 过载响应包含 `Retry-After: 1`，业务 Handler 不会被额外调用。
- 业务执行器可以停止接收新任务，并在关闭阶段安全排空或回收任务。
- 业务隔离和背压测试连续运行 20 轮无失败。

## HTTP 协议与优雅关闭阶段

记录日期：2026-07-22

本阶段保持原有并发性能模型，补充资源边界和连接生命周期验证：

- Header 超限返回 431，Body 超限返回 413，格式错误返回 400，且不会调用业务 Handler。
- 同一 TCP 连接可以连续处理请求，达到单连接请求上限后通过响应头明确关闭。
- Keep-Alive 空闲连接在 `idle_timeout` 后回收。
- 停机时排空已提交的排队与运行请求，并让成功响应携带关闭连接语义。
- 超过 `shutdown_grace_period` 时强制关闭剩余客户端连接，同时继续安全回收正在运行的 Handler。

## 后续验收目标

后续压测与观测阶段应在相同测试方法下继续记录结果，并满足：

- 50 个并发请求继续全部成功。
- 长连接与大量短连接混合场景下不会造成无界内存增长。
- Keep-Alive 不破坏背压、公平性和优雅停机语义。
- 慢连接和耗时业务操作不阻塞其他状态查询。
- macOS、Linux 和 TSan 测试全部通过。
