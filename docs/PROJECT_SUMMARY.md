# Luckfox Pico Firmware – Single Reference (v2.1)

This document replaces the entire previous docs/ tree. It captures the only information still relevant to the trimmed, read-only firmware build that ships the `luckfox_web_config` status monitor.

---

## 1. Scope & Goals
- **Board**: Luckfox Pico Pro Max (RV1106, ARMv7-A, Buildroot rootfs, OEM partition for apps).
- **Services that must stay running**: `rkipc` (RTSP + recording) and the `luckfox_web_config` status monitor.
- **What was removed**: Any HTTP endpoint that edits `/userdata/rkipc.ini`, FPS/bitrate widgets, or restart helpers (`restart_rkipc.sh`). The monitor is now read-only to avoid destabilising recording.
- **Auth**: HTTP Basic Auth (`admin` / `luckfox`). Keep deployment behind a trusted LAN or VPN; HTTP is unencrypted.

---

## 2. Build & Flash Workflow (Host: Ubuntu/WSL)
1. **Prerequisites**
   - SDK at `/home/becube/luckfox-pico` (official Luckfox repo, includes `build.sh`).
   - Toolchain: `/home/becube/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc`.
   - Project repo: `/home/becube/luckfox-pico-test`.
2. **One-command build**
   ```bash
   cd /home/becube/luckfox-pico-test
   ./scripts/build_firmware.sh
   ```
   - Compiles `src/web_config.c` to `luckfox_web_config` (ARM, 32-bit, ~33 KB, MD5 currently `b0e6f1931a463e42347938a5404a8674`).
   - Clears stale rootfs/output, rebuilds rootfs via SDK `build.sh rootfs`.
   - Copies the binary + overlays (init scripts, configs) into the SDK staging folders.
   - Runs `build.sh firmware && build.sh updateimg`, then copies `update.img` to project root as `update_v2.1_YYYYMMDD_HHMMSS_<md5suffix>.img`.
3. **Flash**
   ```bash
   sudo ./scripts/flash_firmware.sh usb      # Rockchip maskrom (rkdeveloptool)
   sudo ./scripts/flash_firmware.sh sdcard   # dd image to SD card
   ```
4. **Post-build sanity**
   ```bash
   ssh root@<ip> "md5sum /oem/usr/bin/luckfox_web_config"   # must match host build
   ls -lh /oem/usr/bin/luckfox_web_config                    # ~33 KB
   ping <ip>; ssh root@<ip> uptime                           # confirm boot
   ```

---

## 3. Runtime Layout on the Board
- **Init scripts**
  - `/etc/init.d/S99luckfox_video` – mounts SD folders, starts `rkipc` then `luckfox_web_config`.
  - `/etc/init.d/S00userdata_init` – first-boot hook; copies the template `rkipc.ini` from OEM and creates `/userdata/.initialized_v2`. Remove that marker to re-run the bootstrap.
- **Config**: `/userdata/rkipc.ini` survives reflashes. Key values that must stay:
  - `[storage] mount_path = /mnt/sdcard`
  - `[storage.0] enable = 1`, `folder_name = recordings`, `file_duration = 180`
  - `[rtsp] port = 554`
  - `[system] timezone = UTC-7` (or change manually)
- **Recording storage**: `/mnt/sdcard/recordings/*.mp4` (3‑minute segments, auto-rotation handled by rkipc quotas). Init script ensures `recordings`, `video1`, `video2` exist with `0777`.
- **Logs**: `luckfox_web_config` appends to `/mnt/sdcard/web_status.log` and rotates into `.old` after 2 MB. Kernel/service logs remain in BusyBox `logread`/`dmesg`.

---

## 4. Web Status Monitor (luckfox_web_config)
- **Binary location**: `/oem/usr/bin/luckfox_web_config` (auto-started). Manual restart: `killall luckfox_web_config; /oem/usr/bin/luckfox_web_config &`.
- **Port**: 8080 (HTTP). Endpoints:
  - `GET /` – dashboard HTML (auto-refresh every 5 s).
  - `GET /api/status` – JSON snapshot (`uptime`, `memory`, SD total/free, board time, `rtsp_running`, `recording_enabled`, `video_count`).
  - `GET /api/logs` – most recent log lines (JSON array).
- **LED logic**: RTSP LED is green only if port 554 listens; Recording LED is red only if `[storage.0] enable=1` *and* the segment counter advanced in the last 5 minutes.
- **Auth enforcement**: Basic Auth headers are validated for every request; unauthenticated calls get HTTP 401.
- **Why read-only?** Writing to `/userdata/rkipc.ini` from the web tier previously caused race conditions with `rkipc` (crashes, stuck recordings). The only supported way to edit camera parameters now is manual SSH + editor.

---

## 5. Manual Configuration & Maintenance
- **Edit config safely**
  ```bash
  ssh root@<ip>
  vi /userdata/rkipc.ini
  # adjust [video.0] or storage quotas as needed
  reboot
  ```
- **Reset config to template**
  ```bash
  ssh root@<ip> "rm -f /userdata/.initialized_v2 /userdata/rkipc.ini && reboot"
  ```
- **Check recording health**
  ```bash
  ls -lh /mnt/sdcard/recordings | tail -3      # new files every 3 minutes
  df -h /mnt/sdcard                            # confirm real SD capacity (~119 G)
  ```
- **Common fixes (already baked into overlays, keep these notes if you debug a live board):**
  1. Recording disabled – `S99luckfox_video` now auto-enforces `enable = 1` and `folder_name = recordings` on every boot.
  2. Wrong mount path – auto-corrected to `/mnt/sdcard` on boot.
  3. Missing folders after inserting a blank SD card – rerun `S99luckfox_video start` to recreate them.
  4. Disk stats wrong – current code shells out to `df -h /mnt/sdcard`; if it shows 3 G, the card was not mounted.

---

## 6. Troubleshooting Checklist
| Symptom | Command | Expected |
|---------|---------|----------|
| Web UI unreachable | `netstat -tuln \| grep 8080` | LISTEN on 0.0.0.0:8080 |
| RTSP LED grey | `netstat -tuln \| grep 554` | LISTEN entry; if missing, restart `rkipc` |
| Recording LED grey | `tail -f /mnt/sdcard/web_status.log` | Look for `segment_count` increments; check SD space |
| **Video not recording** | `grep -A5 "\[storage.0\]" /userdata/rkipc.ini` | `enable = 1`, `folder_name = recordings` |
| Wrong stats | `curl -u admin:luckfox http://<ip>:8080/api/status \| jq .` | Fields match `df`, `free`, `uptime` |
| Need verbose logs | Run the binary manually in foreground to stream stdout |

> **Critical Note on Persistence**: `/userdata/rkipc.ini` is preserved across reboots and some flashes. If recording is disabled, it's likely an old config persisting. Run the reset command in Section 5 to force a refresh.

---

## 7. Optional Modules Still in the Repo
These stay for developers but are **not** part of the shipping firmware path:
- `src/video_stream_record.c` – placeholder app meant to be wired into Rockchip MPP; requires manual integration (see `tools/jtt1078_*`, `docs archive`).
- JT/T 1078 stack (`src/jtt1078_*.c`, plus notes preserved in git history) – provides an encoder that frames rkipc output into JT/T 1078 packets; compile with the same ARM toolchain if needed.
- `tools/jtt1078_server.py` – simple TCP test harness.
If you hook any of these back up, document the change separately; this file intentionally tracks only the supported production slice.

---

## 8. Verification After Flash
1. **Boot & network**: ping board, SSH in (`root` / no password).
2. **Services**:
   ```bash
   ps | grep -E "rkipc|luckfox_web_config"
   ```
3. **Storage prep**:
   ```bash
   ls -ld /mnt/sdcard/recordings /mnt/sdcard/video1 /mnt/sdcard/video2
   ```
4. **Web monitor**: visit `http://<ip>:8080`, confirm both LEDs lit, logs stream.
5. **RTSP stream**: `ffprobe rtsp://<ip>:554/live/0` (should show 2304x1296@25fps unless you changed config manually).
6. **Binary integrity**: `md5sum /oem/usr/bin/luckfox_web_config`.

---

## 9. Intentional Limitations & Future Notes
- No REST control endpoints; configuration must happen over SSH to keep the device stable.
- No websocket or push APIs; polling every 5 s is sufficient and keeps code size minimal.
- HTTPS/TLS is not planned on-device; use a reverse proxy if remote exposure is required.
- Long-form historical docs were moved out; if you need deprecated procedures (old FPS API, JT/T deployment scripts, etc.), recover them from git history.

---

## 10. Quick Reference Commands
```bash
# Build firmware on host
./scripts/build_firmware.sh

# Flash via USB OTG
sudo ./scripts/flash_firmware.sh usb

# Verify services on board
ps | grep luckfox
netstat -tuln | grep -E '8080|554'

# Tail persistent web monitor log
ssh root@<ip> "tail -f /mnt/sdcard/web_status.log"

# Reset first-boot flow
ssh root@<ip> "rm -f /userdata/.initialized_v2 /userdata/rkipc.ini && reboot"
```

This single file is now the authoritative reference; delete or archive older docs instead of updating them piecemeal.
