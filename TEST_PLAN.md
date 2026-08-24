# TEST PLAN — Production-safe read-only monitor

## Release gate

A firmware build is acceptable only when all of the following are true:

- `luckfox_web_config` compiles with `-Wall -Wextra -Werror`.
- HTTP exposes only GET endpoints.
- `POST /api/config` and `POST /api/restart` return `405 Method Not Allowed`.
- A long/malformed `Authorization` header cannot crash the process.
- Removing the SD card is detected from `/proc/mounts`; the monitor must not create a fake write-test file on rootfs.
- Recording status is driven by inotify/cached state, not a full directory scan every second.
- Normal dashboard polling does not append one log line per request.
- Stopping/restarting the web monitor does not stop `rkipc`.
- Existing non-empty `/userdata/recordings` is never deleted automatically.

## Host tests

```bash
make check
make web_config
```

For a native smoke test, temporarily change `WEB_PORT` to a free local port, run the binary, then verify:

```bash
curl -u admin:luckfox http://127.0.0.1:<port>/api/status
curl -u admin:luckfox http://127.0.0.1:<port>/api/logs
curl -i -X POST -u admin:luckfox http://127.0.0.1:<port>/api/status
curl -i http://127.0.0.1:<port>/api/status
```

Expected: status/logs return JSON; POST returns 405; unauthenticated GET returns 401.

## Board tests

### 1. Boot/service integrity

```bash
ps | grep -E 'rkipc|luckfox_web_config'
netstat -lnt | grep -E ':554|:8080'
```

Both services should be present when the SD card is mounted and storage layout is safe.

### 2. RTSP stability soak

Run a 12–24 hour client soak:

```bash
ffprobe -rtsp_transport tcp rtsp://<ip>:554/live/0
```

Watch CPU, memory, dropped frames and service restarts. The monitor must not cause periodic CPU/SD-I/O spikes.

### 3. Recording watcher

Create/close a new recording segment and confirm `video_count` and `last_segment_age_sec` update without rescanning the directory.

### 4. Large directory test

Populate several thousand `.ts`/`.mp4` entries. Dashboard polling must remain roughly constant-cost; no per-request or per-second full-directory walk is allowed.

### 5. SD removal/reinsert

Remove the SD card. `/api/status` must report `sd_status=0`. The application must not create `/mnt/sdcard/.write_test*` on rootfs.

Reinsert/mount the SD and run:

```bash
/etc/init.d/S99luckfox_video start-rkipc
```

if `rkipc` was intentionally held back for storage safety.

### 6. Existing userdata protection

Place a test file in `/userdata/recordings`, reboot/start the init script, and verify the directory is preserved. The script should warn and refuse to replace it automatically.

### 7. Authentication robustness

Send Authorization tokens at 256 B, 1 KiB, 8 KiB and malformed forms. Process must remain alive and return 401.

### 8. Read-only contract

```bash
curl -i -X POST -u admin:luckfox http://<ip>:8080/api/config
curl -i -X POST -u admin:luckfox http://<ip>:8080/api/restart
```

Both must return 405 and `/userdata/rkipc.ini` checksum must remain unchanged.

### 9. Logging wear

Leave the dashboard open for one hour. `web_status.log` should not grow from ordinary 5-second status polls; it should grow only on startup, state transitions and errors.
