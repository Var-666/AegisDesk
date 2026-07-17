# Agent HTTP Server 性能基线

## 目的

本文记录 HTTP Server 并发优化前的性能表现，用于后续异步 I/O 和业务线程池改造的对照。基线测试通过真实 TCP 连接访问绑定到随机端口的 Agent HTTP Server，不使用 Mock 网络层。

当前实现采用单线程同步处理模型：

```text
accept → read → AgentApi handler → write → 下一连接
```

因此，请求数量增加时，总耗时和 P95 延迟会随并发客户端数量近似线性增长。

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

## 后续验收目标

完成异步网络 I/O 和有界业务线程池后，应在相同测试方法下重新记录结果，并满足：

- 50 个并发请求全部成功。
- 50 并发批次耗时不再接近 50 倍单请求业务耗时。
- P95 延迟相对本基线显著下降。
- 慢连接和耗时业务操作不阻塞其他状态查询。
- macOS、Linux 和 TSan 测试全部通过。
