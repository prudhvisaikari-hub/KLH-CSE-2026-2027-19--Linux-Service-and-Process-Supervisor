# Architecture & Design

## 1. Problem Statement

A production Linux server runs many background services (web servers,
database services, monitoring agents, scheduled tasks). If a service
crashes, the system must detect the failure and restart it automatically,
without a human watching a terminal 24/7.

## 2. High-Level Design

```
 services.conf
      │
      ▼
 ┌───────────────┐   fork()+execv()   ┌───────────────┐
 │  supervisord  │ ─────────────────► │  service proc │
 │  (daemon)     │                    │  (child)      │
 │               │                    │               │
 │  SIGCHLD  ◄───┼────── exit/crash───┤               │
 │  waitpid()    │                    └───────────────┘
 │  auto-restart │
 │               │   /tmp/supervisor_cmd.fifo (commands)
 │               │ ◄───────────────────────────────────  svctl (CLI)
 │               │   /tmp/supervisor_reply.fifo (replies)
 │               │ ───────────────────────────────────►  svctl (CLI)
 └───────────────┘
      │
      ▼
 supervisor.log / supervisor_status.txt
```

The system is split into two cooperating programs, mirroring how real
service managers are structured (e.g. `systemd` + `systemctl`,
`supervisord` + `supervisorctl`):

- **`supervisord`** — the long-running daemon that owns and monitors every
  service process.
- **`svctl`** — a lightweight CLI client that sends control commands to
  the daemon and prints its reply. It never touches child processes
  directly; it only talks to `supervisord` over two named pipes (FIFOs).

## 3. Core Mechanisms

| Requirement | Implementation |
|---|---|
| Read service configs | `load_config()` parses `services.conf` (name, path, quoted args, autorestart flag, max_restarts) with a quote-aware tokenizer. |
| Create/start services | `start_service()` calls `fork()` then `execv()` in the child. |
| Monitor child state | A `SIGCHLD` handler sets a flag (async-signal-safe); the main loop calls `waitpid(-1, &status, WNOHANG)` in `reap_children()`. |
| Detect unexpected termination | `WIFEXITED` / `WIFSIGNALED` on the reaped status distinguishes normal exit from a crash/signal kill. |
| Auto-restart failed services | Each `Service` tracks `restart_count` vs. configured `max_restarts`; a crash triggers an immediate `start_service()` call until the cap is hit, after which the service is marked `TERMINATED`. |
| Collect termination info | `waitpid()` return value + `status` macros, as above. |
| Signals to stop/continue/terminate | `SIGTERM` (stop, escalating to `SIGKILL` after a 3s grace period), `SIGSTOP` (pause), `SIGCONT` (resume). |
| Maintain status & logs | `log_msg()` timestamps every event into `supervisor.log`; `write_status_file()` keeps `supervisor_status.txt` current. |
| Controlled shutdown of all services | `shutdown_all()` iterates every service and calls `stop_service()`, triggered by `SHUTDOWN` (via `svctl`), `SIGINT`, or `SIGTERM` to the daemon itself. |
| Display current state | `format_status()` builds a table (service, PID, state, restart count, max restarts) shown via `svctl STATUS`. |

## 4. Concurrency & Signal Safety

- The `SIGCHLD` handler does **no** unsafe work — it only sets a
  `volatile sig_atomic_t` flag. All logging, `waitpid()` calls, and
  restart decisions happen in the main loop, which is the correct pattern
  for async-signal-safety in C.
- The main loop uses `select()` with a 1-second timeout on the command
  FIFO, so it remains responsive to both new child-exit events and
  incoming `svctl` commands without busy-waiting.

## 5. Inter-Process Control Protocol

`svctl` and `supervisord` communicate over two named pipes:

- `/tmp/supervisor_cmd.fifo` — `svctl` writes one command per line
  (`STATUS`, `START <name>`, `STOP <name>`, `RESTART <name>`,
  `PAUSE <name>`, `RESUME <name>`, `SHUTDOWN`).
- `/tmp/supervisor_reply.fifo` — `supervisord` writes back an
  acknowledgement or the status table, which `svctl` prints.

The daemon keeps the command FIFO open for read+write itself
(`O_RDWR`) so it never sees `EOF` when no client is currently writing.

## 6. Limitations & Possible Extensions

See the top-level `README.md` → "Future Work" section.
