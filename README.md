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
This produces five binaries in `src/`: `supervisord`, `svctl`, `webctl`, `httpserver`, and `updater`.

## Run

**Terminal 1 — start the daemon:**
```bash
cd src
./supervisord services.conf
```

**Terminal 2 — control it via CLI:**
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

**Terminal 3 — control it via Web Dashboard:**
```bash
cd src
./webctl
```
Then open your web browser and navigate to `http://localhost:8080`. The web dashboard provides a live, auto-refreshing view of your services and allows you to control them with a click.

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

## Servers and Automated Updates

This project includes real POSIX C network services and an autonomous rolling update agent integrated into the supervisor process tree:

### Component Overview

- **`httpserver` (`src/httpserver.c`)**: A minimal, single-threaded HTTP server built purely with raw POSIX sockets (`socket`, `bind`, `listen`, `accept`).
  - Reads the release version from `<version_file>` once at startup.
  - Serves `GET /health` with plain text `OK name=<name> version=<version> pid=<pid>\n` (HTTP 200). If the version string contains the word `broken`, it simulates an unhealthy release by returning HTTP 503 `UNHEALTHY name=<name> version=<version> pid=<pid>\n`.
  - Serves `GET /` with a dark-themed HTML status page showing service name, release version, health status, port, PID, and uptime, auto-refreshing every 2 seconds.
  - Configured on ports 9001, 9002, and 9003 in `services.conf`:
    - `web1`: port `9001`
    - `web2`: port `9002`
    - `web3`: port `9003`

- **`updater` (`src/updater.c`)**: An automated rolling deployment agent that runs as a managed supervisor service (`./updater deploy/VERSION web1:9001 web2:9002 web3:9003`).
  - Polls `deploy/VERSION` every 2 seconds and detects content changes.
  - Performs a zero-downtime rolling restart: for each target server in order, it executes `./svctl RESTART <name>` via `popen()` and validates the `"-> OK"` reply, then sends `"GET /health HTTP/1.0"` over a raw TCP socket to verify HTTP 200 and `"version=<new version> "`. Retries up to 10 times, 0.5 seconds apart.
  - **Automated Rollback**: If any target server fails its health check, the updater immediately restores the previous release version into `deploy/VERSION`, restarts all servers touched so far back to the old version, verifies their health, and halts rollout. Untouched servers remain completely uninterrupted.
  - Validates service names (alphanumeric, `_`, `-` only) and logs all lifecycle steps with timestamps to both stdout and `updater.log`.

### Publishing a Release

Releases can be published in two ways:

1. **Via the Web Dashboard (`webctl` on port 8080):**
   Open `http://localhost:8080`, enter a new version (e.g. `2.0`) into the **Publish release** form, and submit.
   Or use `curl`:
   ```bash
   curl -X POST localhost:8080/release -d "version=2.0"
   ```
   *Note: Versions are strictly validated to 1–32 characters containing only letters, digits, '.', '_', and '-'. Invalid input is rejected with HTTP 400.*

2. **Via the Filesystem:**
   Directly write to the version file:
   ```bash
   echo "2.0" > deploy/VERSION
   ```
   Within 2 seconds, the `updater` agent detects the new version and performs a rolling restart across `web1`, `web2`, and `web3`.

### Demonstrating an Automated Rollback

To test automated failure detection and instantaneous rollback:
1. Publish a release containing `broken` (e.g. `broken-3.0`):
   ```bash
   # Via CLI:
   echo "broken-3.0" > deploy/VERSION

   # Or via Web Dashboard / curl:
   curl -X POST localhost:8080/release -d "version=broken-3.0"
   ```
2. The `updater` restarts `web1`. Upon querying `http://localhost:9001/health`, `web1` returns HTTP 503 (`UNHEALTHY`).
3. After 10 failed retries, `updater` triggers an automated rollback:
   - Restores `deploy/VERSION` back to `2.0`.
   - Restarts `web1` back to `2.0` and confirms it is healthy.
   - Leaves `web2` and `web3` untouched and online.
   - Logs `HEALTH CHECK FAILED on 'web1'` followed by `ROLLBACK COMPLETE: restored 2.0` in `updater.log`.

### Limitations

- **Single-Threaded Architecture**: Both `httpserver` and `webctl` use single-threaded POSIX socket loops with client receive timeouts (2 seconds) designed for simplicity and educational clarity.
- **Demo Switch**: The word `"broken"` in a version string serves as an intentional demo switch to simulate bad releases without injecting runtime crashes.

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
