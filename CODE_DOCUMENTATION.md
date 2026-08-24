# Code documentation — supported production path

## `src/web_config.c`

`web_config.c` is a small read-only HTTP/status daemon. It is intentionally independent from the Rockchip media pipeline.

### Threads

- **main thread** — HTTP listener and request dispatch.
- **status thread** — refreshes RTSP, SD, memory, uptime and storage cache every 5 seconds.
- **recording watcher** — inotify watcher for segment lifecycle events.
- **LED thread** — reflects cached recording/SD/RTSP state on GPIO.

The LED thread never calls RTSP/storage probes itself.

### Shared state

`camera_status_t` is protected by `status_mutex`. Recording watcher counters/timestamps are also protected by the same mutex. HTTP handlers copy one snapshot and serialize that copy.

### Recording watcher

`initial_recording_scan()` performs a single seed scan when an inotify watch is attached. Supported video suffixes are `.ts`, `.mp4`, `.mkv`, `.h264`, `.h265`.

After the seed scan:

- `IN_CREATE` / `IN_MOVED_TO` increment the segment count;
- `IN_DELETE` / `IN_MOVED_FROM` decrement it;
- `IN_CLOSE_WRITE` / `IN_MOVED_TO` refresh the last-segment monotonic timestamp.

Unmount/watch invalidation causes a delayed reattach and a fresh seed scan.

### RTSP health

`probe_tcp_port_local()` opens a non-blocking TCP socket to loopback port 554 and waits up to 300 ms. It does not fork a shell or run `netstat`, `ss` or `pgrep`.

### SD health

`get_sd_mount_status()` parses `/proc/mounts` for the exact `/mnt/sdcard` mountpoint and returns missing/RO/RW state. `statvfs()` is used only on a confirmed mount.

### HTTP parser

The server reads until `\r\n\r\n` across multiple `recv()` calls with a 16 KiB cap and socket timeouts. It accepts GET only. Responses carry `Content-Length` and use `send_all()`.

### Authentication

The Basic Auth token is compared in-place against the expected token. There is no fixed-size destination buffer for attacker-controlled Authorization data, removing the previous overflow.

### Logging

The logger is mutex-protected and rotates at 2 MiB. Ordinary requests are not logged. `/api/logs` returns the tail of the persistent/fallback log with JSON escaping.

## `overlay/etc/init.d/S00userdata_init`

Creates first-boot state without overwriting an existing `rkipc.ini`.

## `overlay/etc/init.d/S99luckfox_video`

Prepares storage without deleting non-empty userdata, ensures an existing `rkipc` is left alone, and manages only the web monitor on stop/restart.

## `scripts/build_firmware.sh`

Cross-compiles with warnings-as-errors, verifies architecture/checksums, stages the init scripts/config, packages the image and prints reproducible verification commands.

## Non-shipping sources

`src/old_clean.c`, `src/video_stream_record.c`, JT/T 1078 examples and hardware test files are developer experiments unless explicitly wired into the build. They do not define behavior of the supported status-monitor firmware.
