# Production-safe RV1106 firmware reference

This document is the supported runtime reference for the shipping path of this repository.

## 1. Architecture

```text
camera sensor
     |
     v
   rkipc -----------------------> RTSP :554
     |
     +--------------------------> /mnt/sdcard/recordings
                                      |
                                      | inotify
                                      v
                              luckfox_web_config
                                      |
                                      +--> cached status
                                      +--> LEDs
                                      +--> HTTP :8080
```

`rkipc` owns ISP, encoding, RTSP and recording. `luckfox_web_config` only observes state.

## 2. Read-only contract

The monitor never:

- writes `/userdata/rkipc.ini`;
- kills or restarts `rkipc`;
- changes FPS, bitrate, codec or resolution;
- runs `sed` against camera configuration;
- creates SD write-test files;
- scans the entire recording directory every second.

Supported HTTP endpoints:

- `GET /`
- `GET /api/status`
- `GET /api/logs`
- `GET /healthz`

Any non-GET request returns HTTP 405.

## 3. Cached status

The monitor refreshes low-cost system state every 5 seconds.

RTSP uses a non-blocking connection to `127.0.0.1:554`, avoiding `system("netstat ...")`, `ss` and `pgrep`.

Recording uses Linux inotify. At watcher attach time there is one directory scan to seed `video_count` and the newest segment timestamp. After that, create/delete/close/move events update the cached state in O(1) per filesystem event.

Recording is considered active when:

- RTSP is reachable;
- `/mnt/sdcard` is a confirmed read-write mount;
- a video segment was closed/moved into the recording directory within the last 300 seconds.

Monotonic time is used for the running timeout so NTP/wall-clock changes do not create false timeouts.

## 4. SD handling

SD presence is verified by matching `/mnt/sdcard` in `/proc/mounts`. Merely having a directory named `/mnt/sdcard` is not treated as a mounted SD card.

No periodic create/unlink test is performed. Storage size uses `statvfs()` only after the mount is confirmed.

Status values:

- `sd_status=0`: not mounted
- `sd_status=1`: mounted read-only
- `sd_status=2`: mounted read-write

## 5. Logging

Primary log:

```text
/mnt/sdcard/web_status.log
```

Fallback while SD is unavailable:

```text
/tmp/web_status.log
```

The logger rotates at 2 MiB. It logs startup, errors and service/storage transitions. Normal `/api/status` polling is intentionally not logged, reducing SD metadata/data writes.

When persistent storage becomes available, new log events migrate to the SD-backed log.

## 6. HTTP/security behavior

Basic Auth remains enabled for compatibility (`admin` / `luckfox` by default). Authorization parsing performs a bounded exact comparison and does not copy an attacker-controlled token into a fixed stack buffer.

The request reader accepts headers across multiple TCP `recv()` calls up to a fixed 16 KiB limit. Client read/write timeouts are 5 seconds. Responses include `Content-Length` and use a `send_all()` loop to handle partial writes.

The service ignores SIGPIPE and shuts down cooperatively on SIGINT/SIGTERM.

Because Basic Auth is carried over plain HTTP, keep port 8080 on a trusted network or place TLS/VPN in front of it.

## 7. Init scripts

### S00userdata_init

Runs once. If `/userdata/rkipc.ini` already exists, it is preserved exactly. The OEM template is copied only when the config is missing.

### S99luckfox_video

Normal `start` never rewrites config and never kills an existing `rkipc`.

Before starting a new `rkipc`, it verifies the SD mount and prepares `/userdata/recordings -> /mnt/sdcard/recordings`.

If `/userdata/recordings` is a non-empty real directory, it is preserved and startup is refused instead of deleting data. Recover/move the data manually, then run:

```sh
/etc/init.d/S99luckfox_video start-rkipc
```

`stop`/`restart` manage the monitor only, so web maintenance cannot interrupt RTSP/recording.

## 8. Configuration

Camera parameters live only in:

```text
/userdata/rkipc.ini
```

The monitor does not assume or force a particular recording container, segment duration, resolution or bitrate. This avoids documentation/runtime drift when those camera settings are intentionally changed.

To make a change:

```bash
ssh root@<ip>
vi /userdata/rkipc.ini
reboot
```

Use the checksum of the config before/after web testing to prove the web monitor is read-only.

## 9. Build

```bash
./scripts/build_firmware.sh
```

Useful overrides:

```bash
SDK_PATH=/opt/luckfox-pico CLEAN_BUILD=0 ./scripts/build_firmware.sh
```

The build script uses a single compile policy matching the Makefile:

```text
-std=c11 -O2 -Wall -Wextra -Werror -pthread
```

It verifies ARM 32-bit output and prints both MD5 and SHA-256 for the binary/firmware.

## 10. Release testing

Use `TEST_PLAN.md`. The main gates are:

- 12–24 hour RTSP soak;
- large recording directory test;
- SD remove/reinsert;
- long malformed Authorization headers;
- POST read-only contract;
- no log growth from ordinary polling;
- preservation of existing `/userdata/recordings`.
