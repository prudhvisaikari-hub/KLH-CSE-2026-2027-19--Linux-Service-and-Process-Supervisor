# Results

This folder holds evidence of the working system: logs, status snapshots,
and console output captured from actual test runs. Each run gets its own
timestamped subfolder so results are never overwritten between reviews.

## sample_run_2026-09-20/

A full demonstration session showing every required capability in one run:

| File                    | What it shows |
|-------------------------|----------------|
| `console_output.log`    | Full stdout of `./supervisord services.conf`, including startup, all four services launching, and shutdown. |
| `supervisor.log`        | Timestamped activity log: service starts, crash detection, auto-restart attempts, restart-limit enforcement, pause/resume, and graceful shutdown. |
| `supervisor_status.txt` | Snapshot of `svctl STATUS` output at the moment of shutdown. |

### What this run demonstrates

1. **Startup** — all 4 configured services (`webserver`, `flaky_task`,
   `monitor`, `backup_job`) launched via `fork()`/`execv()`.
2. **Crash detection & auto-restart** — `flaky_task` is configured to exit
   after ~2 seconds; the supervisor detects this via `SIGCHLD`/`waitpid()`
   and restarts it automatically.
3. **Restart-limit enforcement** — after 3 restart attempts (its configured
   `max_restarts`), `flaky_task` is marked `TERMINATED` and is **not**
   restarted again.
4. **One-shot job handling** — `backup_job` (`autorestart=0`) runs once,
   finishes, and correctly stays `STOPPED` rather than being relaunched.
5. **Runtime control** — `PAUSE monitor` (SIGSTOP) followed by
   `RESUME monitor` (SIGCONT), issued live via `svctl`.
6. **Graceful shutdown** — `svctl SHUTDOWN` triggers `SIGTERM` to every
   running service and a clean daemon exit, with no orphaned processes.

## Adding new results

Each subsequent test run (per review milestone) should be added as a new
`sample_run_<date>/` folder rather than overwriting this one, so the
evaluation history is preserved across the project's lifetime.
