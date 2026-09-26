# Linux Service & Process Supervisor

A lightweight, dependency-free C implementation of a service/process
supervisor for Linux — in the spirit of `supervisord` or `systemd`, but
built directly on core POSIX primitives (`fork`, `exec`, `waitpid`,
signals) for learning and demonstration purposes.

## Abstract

Production Linux servers run many background services — web servers,
databases, monitoring agents, scheduled jobs. When one of these crashes,
someone (or something) has to notice and restart it, ideally without a
human babysitting a terminal. This project implements a service
supervisor that reads service definitions from a configuration file,
launches each one with `fork()`/`execv()`, and continuously monitors
them using a `SIGCHLD` handler paired with `waitpid()`. If a service
exits unexpectedly, it is automatically relaunched up to a configurable
retry limit; beyond that limit it is marked `TERMINATED` rather than
restarted indefinitely. A companion command-line tool (`svctl`) lets an
operator start, stop, restart, pause (`SIGSTOP`), or resume (`SIGCONT`)
any service at runtime, and view a live status table — all communicated
to the daemon over named pipes (FIFOs). Every event is timestamped and
logged, and a controlled, graceful shutdown (`SIGTERM`, escalating to
`SIGKILL` if needed) stops every managed service cleanly on request.

## Repository Structure

```
.
├── README.md              # this file
├── .gitignore
├── src/                   # all source code
│   ├── supervisor.c        # the supervisord daemon
│   ├── svctl.c              # control CLI client
│   ├── Makefile
│   └── services.conf        # example service configuration
├── docs/
│   └── architecture.md     # system design, mechanisms, IPC protocol
├── data/                  # data source reference (see data/README.md)
└── results/               # captured logs/output from real test runs
    └── sample_run_2026-09-20/
```

## Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential
```

## Build

```bash
cd src
make
```
This produces two binaries in `src/`: `supervisord` and `svctl`.

## Run

**Terminal 1 — start the daemon:**
```bash
cd src
./supervisord services.conf
```

**Terminal 2 — control it:**
```bash
cd src
./svctl STATUS
./svctl STOP webserver
./svctl START webserver
./svctl RESTART flaky_task
./svctl PAUSE monitor
./svctl RESUME monitor
./svctl SHUTDOWN
```

Logs are written to `src/supervisor.log`; the live status snapshot is
`src/supervisor_status.txt`. Full details, the config file format, and
an architecture diagram are in `docs/architecture.md`. Sample output
from a real run is in `results/`.

## Clean up

```bash
cd src
make clean
```

## Configuration file format

```
# name  path  "arguments"  autorestart(0|1)  max_restarts
webserver /bin/sh "-c 'while true; do echo hi; sleep 5; done'" 1 5
```

- **name** — unique identifier used with `svctl` to target a service.
- **path** — full path to the executable (no `$PATH` search, matching
  `execv()`).
- **"arguments"** — quoted argument string; single quotes inside it are
  honoured, so an inline shell command like `-c 'do a; do b'` is safe.
- **autorestart** — `1` to relaunch automatically on crash/exit, `0` for
  one-shot/scheduled jobs that should stay stopped once finished.
- **max_restarts** — cap on automatic restarts before the service is
  marked `TERMINATED` and left alone.

Lines starting with `#` and blank lines are ignored.

## Core capabilities

| Requirement | Implementation |
|---|---|
| Read service configs | `load_config()` parses `services.conf` with a quote-aware tokenizer. |
| Create/start services | `fork()` + `execv()` in `start_service()`. |
| Monitor child state | `SIGCHLD` handler sets a flag; main loop reaps with `waitpid(-1, &status, WNOHANG)`. |
| Detect unexpected termination | `WIFEXITED` / `WIFSIGNALED` on the reaped status. |
| Auto-restart failed services | Per-service `restart_count` vs. `max_restarts`, enforced in `reap_children()`. |
| Signals for control | `SIGTERM` (stop, escalating to `SIGKILL`), `SIGSTOP` (pause), `SIGCONT` (resume). |
| Status & logging | Timestamped `supervisor.log`; live `supervisor_status.txt`. |
| Controlled shutdown | `SHUTDOWN` command, `Ctrl+C`, or `SIGTERM` to the daemon stop every managed service gracefully. |
| Display current state | `svctl STATUS` — table of service, PID, state, restart count, max restarts. |

## Future Work

- True daemonization (double-fork, detach from controlling terminal).
- Per-service log files instead of a shared daemon log.
- Exponential backoff between restart attempts.
- A `RELOAD` command to re-read `services.conf` without restarting
  already-running services.
- Per-service resource limits via `setrlimit()`.

## License

Add a license of your choice (e.g. MIT) if you plan to share or publish
this project publicly.
