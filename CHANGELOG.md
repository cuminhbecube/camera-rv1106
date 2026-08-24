# Changelog

## v2.2 — Production-safe read-only monitor

### Safety and stability

- Removed all web configuration writes and `rkipc` restart controls.
- Removed web-startup configuration migration.
- Removed boot-time repeated `sed` enforcement of `/userdata/rkipc.ini`.
- Replaced per-second recording directory scans with inotify.
- Replaced SD write-test files with `/proc/mounts` detection.
- Replaced `netstat`/`ss`/`pgrep` shell calls with a cached non-blocking RTSP TCP probe.
- Converted LED logic to cached status only.
- Reduced persistent logging to startup, state changes and errors.
- Added `/api/logs` and `/healthz`.

### Security and HTTP correctness

- Removed the Authorization fixed-buffer overflow.
- Added bounded multi-recv HTTP header handling.
- Added 5-second socket timeouts.
- Added `Content-Length`, security response headers and partial-write-safe `send_all()`.
- Non-GET methods now return HTTP 405.
- SIGPIPE is ignored; SIGINT/SIGTERM use cooperative shutdown.

### Boot/data protection

- `S00userdata_init` preserves an existing `rkipc.ini`.
- `S99luckfox_video` never deletes a non-empty `/userdata/recordings`.
- Normal service start does not kill a healthy `rkipc`.
- Web stop/restart does not stop the camera pipeline.
- `rkipc` startup is held back when the SD recording path is unsafe.

### Build and docs

- Unified Makefile/build flags.
- Added warnings-as-errors to the production compile.
- Added `SDK_PATH`, `TOOLCHAIN`, `CC` and `CLEAN_BUILD` overrides.
- Added SHA-256 output alongside MD5.
- Replaced the stale test plan.
- Updated README, project summary, code documentation and safety review.

## v2.1 and earlier

Historical development included writable FPS/bitrate/resolution/config endpoints, automatic config enforcement and experimental JT/T 1078 modules. Those writable web-control paths are no longer part of the supported production firmware.
