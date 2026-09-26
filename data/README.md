# Data

This project does not consume an external dataset. Its "data" is a
runtime **configuration file** describing the services to be supervised.

- **Source**: `../src/services.conf` — hand-authored, not an
  external/licensed dataset.
- **Format**: plain text, one service per line
  (`name path "arguments" autorestart max_restarts`). Documented in the
  top-level `README.md` and `../docs/architecture.md`.

If a later phase of this project adds real datasets (e.g. sample logs to
replay, benchmark workloads, etc.), place them here and document:
- the source/origin of the data,
- its license or usage terms,
- and avoid committing confidential or licensed data directly to
  version control. Large or restricted datasets should instead be
  referenced here by a documented external link or retrieval script.
