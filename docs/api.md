# Lifecycle Status API

`GET /api/v1/services` and `GET /api/v1/services/{service_id}/status` share the same lifecycle fields. Start, stop, and restart responses return the same status shape.

## Compatibility

The original v1 fields remain unchanged: `state`, `desired_state`, `pid`, `uptime_seconds`, and `last_exit_code`. Process Supervisor 2.0 only appends fields, so an existing client can ignore them.

| Field | Type | Meaning |
| --- | --- | --- |
| `process_group_id` | integer | Managed PGID while running; `-1` when no process group is owned |
| `last_exit_kind` | string | `none`, `exited`, `signaled`, or `unknown` |
| `last_exit_signal` | integer or `null` | POSIX signal number when `last_exit_kind` is `signaled` |
| `last_error` | string | Most recent lifecycle error; empty after a successful transition |
| `last_transition_at_unix_ms` | integer | Unix epoch milliseconds of the last lifecycle transition |

`last_exit_code` is retained for compatibility. For a signal exit it is `128 + signal`; new clients should use `last_exit_kind` and `last_exit_signal` when presenting the reason.

## Desktop behavior

The Desktop normalizes state names only for control decisions. It displays the server value, process group, exit reason, error and local-time transition timestamp. All lifecycle controls are disabled while the state is `Starting` or `Stopping`, or while an action request is in flight.
