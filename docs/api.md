# 生命周期状态 API

`GET /api/v1/services` 与 `GET /api/v1/services/{service_id}/status` 使用相同的生命周期字段。启动、停止和重启接口也返回同样结构的状态信息。

## 兼容性

原有 v1 字段保持不变，包括 `state`、`desired_state`、`pid`、`uptime_seconds` 和 `last_exit_code`。Process Supervisor 2.0 只追加新字段，因此已有客户端可以安全忽略这些字段。

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `process_group_id` | 整数 | 服务运行时受管进程组的 PGID；当前未持有进程组时为 `-1` |
| `last_exit_kind` | 字符串 | 取值为 `none`、`exited`、`signaled` 或 `unknown` |
| `last_exit_signal` | 整数或 `null` | 当 `last_exit_kind` 为 `signaled` 时，记录对应的 POSIX 信号编号 |
| `last_error` | 字符串 | 最近一次生命周期错误；状态成功转换后为空字符串 |
| `last_transition_at_unix_ms` | 整数 | 最近一次生命周期状态转换对应的 Unix 毫秒时间戳 |

为了保持兼容性，接口继续保留 `last_exit_code`。进程因信号退出时，该字段值为 `128 + signal`；新版客户端在展示退出原因时，应优先使用 `last_exit_kind` 和 `last_exit_signal`。

## Desktop 行为

Desktop 只在判断控制按钮状态时规范化状态名称。界面会直接展示服务端返回的状态值、进程组、退出原因、错误信息和转换时间，并将时间戳转换为本地时间。

服务处于 `Starting` 或 `Stopping` 状态，或者当前仍有控制请求正在执行时，Desktop 会禁用全部生命周期控制按钮，避免重复操作。
