# Architecture

```mermaid
flowchart LR
    Desktop["Qt Desktop\nservice controls and diagnostics"]
    API["HTTP JSON API\nbackward-compatible v1 fields"]
    Registry["Service Registry\nconfig and ownership"]
    Supervisor["Process Supervisor\nstate + desired state"]
    Handshake["Startup Handshake\nclose-on-exec error pipe"]
    Observer["Exit Observer\nsole waitpid owner"]
    Group["Process Group\nleader + descendants"]
    Metrics["Metrics Collector"]
    Health["Health / Alerts"]
    Recovery["Recovery Manager\nbackoff + restart budget"]

    Desktop <-->|"GET / POST"| API
    API --> Registry
    Registry --> Supervisor
    Supervisor --> Handshake
    Handshake --> Group
    Group --> Observer
    Observer --> Supervisor
    Registry --> Metrics
    Metrics --> Health
    Supervisor --> Health
    Health --> Recovery
    Recovery --> Supervisor
    Metrics --> API
    Health --> API
```

## Concurrency ownership

| Concern | Owner |
| --- | --- |
| Start/stop/restart serialization | Per-service operation mutex |
| Lifecycle snapshot | Per-service state mutex |
| Child reaping | Per-run observer thread |
| Stop completion notification | Observer + condition variable |
| Health/recovery scheduling | Health monitor and recovery manager |
| Descendant cleanup | Dedicated process group |

This separation prevents duplicate children, competing `waitpid` calls, long-held state locks, and zombie processes.
