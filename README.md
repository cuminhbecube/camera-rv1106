# Luckfox Pico Pro Max Camera Firmware

Production-oriented firmware overlay for the Luckfox Pico Pro Max (RV1106). The camera pipeline remains owned by Rockchip `rkipc`; the custom `luckfox_web_config` process is a **read-only status monitor**.

## Safety model

The web monitor intentionally cannot change camera parameters or restart `rkipc`.

It exposes only:

| Endpoint | Method | Purpose |
|---|---|---|
| `/` | GET | Status dashboard |
| `/api/status` | GET | Cached system/camera status JSON |
| `/api/logs` | GET | Recent event log as a JSON array |
| `/healthz` | GET | 200 when RTSP is up and the SD card is mounted read-write |

All other methods return `405 Method Not Allowed`. There is no `/api/config` write path and no `/api/restart`.

HTTP Basic Authentication is enabled with the current default credentials `admin` / `luckfox`. HTTP is unencrypted, so deploy the camera only on a trusted LAN/VLAN or behind a VPN/reverse proxy.

## Monitoring implementation

The monitor is designed to stay out of the video path:

- RTSP health: non-blocking local TCP probe of port 554 every 5 seconds.
- Recording health: `inotify` events on `/mnt/sdcard/recordings`; no full directory scan every second.
- Recording count: one initial scan, then event-driven increments/decrements.
- SD health: exact mount lookup in `/proc/mounts`; no periodic create/delete write-test files.
- Storage usage: `statvfs()` only after the SD mount is confirmed.
- System status: cached uptime/memory/time snapshot.
- LEDs: read cached status only; LED updates do not execute shell commands or touch the SD card.
- Logging: startup, state transitions and errors only. Ordinary dashboard polling is not logged.

## Boot ownership

`/etc/init.d/S00userdata_init` performs one-time userdata initialization and never overwrites an existing `/userdata/rkipc.ini`.

`/etc/init.d/S99luckfox_video`:

1. waits briefly for `/mnt/sdcard`;
2. creates SD recording folders;
3. creates safe `/userdata/...` symlinks only when it can do so without deleting existing data;
4. starts `rkipc` only if it is not already running and the recording path is safe;
5. starts the read-only monitor.

The init script does **not** use `sed` to rewrite `rkipc.ini`, does not kill a healthy `rkipc` on normal start, and does not delete a non-empty `/userdata/recordings` directory.

If storage is fixed or inserted after boot:

```sh
/etc/init.d/S99luckfox_video start-rkipc
```

## Build

Default SDK path:

```text
/home/becube/luckfox-pico
```

Override it without editing scripts:

```bash
SDK_PATH=/path/to/luckfox-pico ./scripts/build_firmware.sh
```

Build:

```bash
./scripts/build_firmware.sh
```

The builder compiles `src/web_config.c` with `-std=c11 -O2 -Wall -Wextra -Werror -pthread`, verifies ARM/32-bit output, stages overlays, rebuilds the firmware image, and prints MD5/SHA-256 checksums.

For a quick host syntax check:

```bash
make check
```

## After flashing

```bash
ps | grep -E 'rkipc|luckfox_web_config'
netstat -lnt | grep -E ':554|:8080'

curl -u admin:luckfox http://<ip>:8080/api/status
curl -u admin:luckfox http://<ip>:8080/api/logs

ffprobe -rtsp_transport tcp rtsp://<ip>:554/live/0
```

The persistent camera configuration remains `/userdata/rkipc.ini`. Its recording format, duration, bitrate, resolution and other camera parameters are not overridden by the web monitor.

See `docs/PROJECT_SUMMARY.md` and `TEST_PLAN.md` for runtime details and release-gate tests.
