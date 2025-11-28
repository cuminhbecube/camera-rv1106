# 📚 Tài Liệu Mã Nguồn - Luckfox Camera RV1106

## 📋 Mục Lục
1. [Tổng Quan Hệ Thống](#1-tổng-quan-hệ-thống)
2. [Cấu Trúc Thư Mục](#2-cấu-trúc-thư-mục)
3. [Chi Tiết Từng Module](#3-chi-tiết-từng-module)
4. [Luồng Hoạt Động](#4-luồng-hoạt-động)
5. [Các Vấn Đề Đã Giải Quyết](#5-các-vấn-đề-đã-giải-quyết)

---

## 1. Tổng Quan Hệ Thống

### 🎯 Mục Đích
Firmware tùy chỉnh cho camera Luckfox Pico Pro Max (RV1106) với các tính năng:
- Web interface để giám sát và cấu hình
- Tự động ghi video lên SD card
- RTSP streaming
- Quản lý timezone chính xác (UTC+7 - Việt Nam)
- Định dạng video .ts chống corrupt

### 🏗️ Kiến Trúc
```
┌─────────────────────────────────────────────┐
│           Web UI (Port 8080)                │
│    (HTML/CSS/JS - Dark Mode)                │
└────────────────┬────────────────────────────┘
                 │ HTTP/REST API
┌────────────────▼────────────────────────────┐
│      luckfox_web_config (C Binary)          │
│  - Status monitoring                        │
│  - Config management                        │
│  - Memory calculation                       │
└────────────────┬────────────────────────────┘
                 │ Read/Write
┌────────────────▼────────────────────────────┐
│         /userdata/rkipc.ini                 │
│  (Main configuration file)                  │
└────────────────┬────────────────────────────┘
                 │ Read by
┌────────────────▼────────────────────────────┐
│            rkipc (RTSP Server)              │
│  - Video encoding (H.265)                   │
│  - RTSP streaming (Port 554)                │
│  - Video recording                          │
└────────────────┬────────────────────────────┘
                 │ Writes to
┌────────────────▼────────────────────────────┐
│      /mnt/sdcard/recordings/*.ts            │
│  (Video files in MPEG-TS format)            │
└─────────────────────────────────────────────┘
```

### 🔧 Platform Specs
- **SoC**: Rockchip RV1106 (ARM Cortex-A7, 32-bit)
- **RAM**: 64MB
- **Camera**: SC3336 3MP
- **OS**: Buildroot Linux (uClibc)
- **SDK**: Luckfox Pico SDK

---

## 2. Cấu Trúc Thư Mục

```
luckfox-pico-test/
├── src/                              # Source code
│   ├── web_config.c                  # ⭐ Web server chính
│   ├── video_stream_record.c         # Video recording daemon (legacy)
│   └── [test files]                  # I2C, SPI, UART tests
│
├── overlay/                          # Files được copy vào firmware
│   ├── etc/
│   │   ├── TZ                        # Timezone file (ICT-7)
│   │   ├── init.d/
│   │   │   └── S99luckfox_video      # ⭐ Init script chính
│   │   └── profile.d/
│   │       └── timezone.sh           # Auto-load TZ environment
│   ├── oem/etc/
│   │   └── rkipc.ini.template        # Config template
│   └── userdata/
│       └── rkipc.ini                 # ⭐ Main config file
│
├── scripts/
│   ├── build_firmware.sh             # ⭐ Build script chính
│   └── flash_firmware.sh             # Flash helper
│
├── docs/
│   └── PROJECT_SUMMARY.md            # Technical documentation
│
├── Makefile                          # Build configuration
├── README.md                         # User guide
└── CHANGELOG.md                      # Version history
```

---

## 3. Chi Tiết Từng Module

### 📄 `src/web_config.c` - Web Server & API

#### Mục Đích
HTTP server cung cấp:
- Web UI để giám sát hệ thống
- REST API để đọc/ghi cấu hình
- Tính toán memory chính xác
- Quản lý timezone

#### Các Function Chính

##### 1. `get_memory()` - Tính Toán RAM
```c
void get_memory(char *buffer)
```
**Chức năng:**
- Đọc từ `/proc/meminfo` để lấy MemTotal và MemAvailable
- Tính used = total - available
- Format: "22M / 54M (40%)"

**Tại sao dùng /proc/meminfo?**
- `sysinfo()` không có MemAvailable
- MemAvailable chính xác hơn (kernel tính cả reclaimable cache)
- Tránh hiển thị RAM "đầy" sai

##### 2. `get_rtsp_status()` - Kiểm Tra RTSP
```c
int get_rtsp_status()
```
**Chức năng:**
- Kiểm tra port 554 có listening không
- Fallback: kiểm tra process `rkipc`
- Return: 1 = running, 0 = stopped

**Tại sao check port thay vì process?**
- Reliable hơn (process có thể running nhưng chưa bind port)
- `pgrep` có thể match sai tên

##### 3. `get_recording_status()` - Kiểm Tra Recording
```c
int get_recording_status()
```
**Chức năng:**
- Scan `/mnt/sdcard/recordings/`
- Tìm file mới nhất (mtime)
- Return 1 nếu file được sửa trong 5 phút gần đây

**Tại sao check file thay vì config?**
- Config chỉ cho biết "nên record", không phải "đang record"
- File mtime = chứng cứ thực sự đang ghi

##### 4. `get_sd_status()` - Kiểm Tra SD Card
```c
int get_sd_status()
```
**Chức năng:**
- Check mount point `/mnt/sdcard` tồn tại
- Tạo file test để verify write permission
- Return: 0 = unmounted, 1 = read-only, 2 = read-write

##### 5. `handle_config_update()` - Cập Nhật Config
```c
void handle_config_update(int sock, const char *body)
```
**Chức năng:**
- Parse JSON từ POST request
- Validate giá trị (duration 10-600s, bitrate 512-8192 kbps, etc.)
- Lock file `/userdata/rkipc.ini` (flock)
- Update từng field bằng sed
- **TỰ ĐỘNG RESTART rkipc** để áp dụng config mới

**Tại sao dùng sed thay vì rewrite file?**
- Giữ nguyên format và comments
- Chỉ thay đổi giá trị cần thiết
- Ít risk corrupt file hơn

##### 6. `send_html()` - Web UI
```c
void send_html(int sock)
```
**Chức năng:**
- Generate HTML page với JavaScript embedded
- Dark theme, responsive layout
- Auto-refresh mỗi 5 giây
- LED indicators (green/red/gray)

#### Security
- **Basic Authentication**: Header `Authorization: Basic YWRtaW46bHVja2ZveA==`
- **Read-only by default**: Chỉ admin có thể POST config
- **File locking**: Prevent concurrent config edits

---

### 📄 `overlay/etc/init.d/S99luckfox_video` - Boot Script

#### Mục Đích
Khởi động services khi boot và enforce cấu hình

#### Luồng Hoạt Động

```bash
Boot → init → S99luckfox_video start
         ↓
    1. Set TZ='ICT-7'
         ↓
    2. Tạo thư mục recordings
         ↓
    3. Fix /userdata/rkipc.ini:
       - mount_path = /mnt/sdcard
       - storage.0.enable = 1
       - file_format = ts
       - network.ntp.enable = 0  ← ⭐ CRITICAL
         ↓
    4. Fix template /oem/etc/rkipc.ini.template
         ↓
    5. Kill old rkipc process
         ↓
    6. Start rkipc với TZ='ICT-7'
         ↓
    7. Start luckfox_web_config
         ↓
    8. Done
```

#### Các Sed Commands Quan Trọng

##### 1. Enable Recording
```bash
sed -i '/^\[storage\.0\]/,/^\[/ s/^enable[[:space:]]*=.*/enable = 1/' "$INI_FILE"
```
**Giải thích:**
- `/^\[storage\.0\]/,/^\[/`: Range từ [storage.0] đến section tiếp theo
- `s/^enable.*=.*/enable = 1/`: Replace dòng enable = anything → enable = 1

##### 2. Change File Format to .ts
```bash
sed -i 's/^file_format[[:space:]]*=.*$/file_format = ts/' "$INI_FILE"
```
**Giải thích:**
- Match `file_format = mp4` hoặc `file_format = mp4 ; flv,ts`
- Replace toàn bộ dòng → `file_format = ts`

##### 3. ⭐ Disable NTP (QUAN TRỌNG NHẤT)
```bash
sed -i '/^\[network\.ntp\]/,/^\[/ s/^enable[[:space:]]*=.*$/enable = 0/' "$INI_FILE"
```
**Tại sao?**
- rkipc có NTP client tự động sync time mỗi 60 giây
- NTP set system time về UTC, làm mất timezone offset
- → Giờ đúng 1 phút rồi sai (vì NTP reset)
- Disable NTP = timezone stable

---

### 📄 `overlay/etc/profile.d/timezone.sh` - Auto-load TZ

#### Mục Đích
Export TZ environment variable cho mọi shell session

#### Code
```bash
#!/bin/sh
if [ -f /etc/TZ ]; then
    export TZ=$(cat /etc/TZ)
fi
```

#### Tại Sao Cần?
- File `/etc/TZ` chỉ là text file, không tự động set environment
- Mọi process (rkipc, web_config, shell) cần inherit TZ từ parent
- `/etc/profile.d/*.sh` được source tự động khi login

---

### 📄 `overlay/userdata/rkipc.ini` - Main Config

#### Cấu Trúc
```ini
[storage.0]
enable = 1                          # Bật recording
folder_name = recordings            # Tên thư mục
file_duration_s = 180               # 3 phút/file
file_format = ts                    # ⭐ MPEG-TS format

[video.0]
width = 2304                        # 2304x1296 (3MP)
height = 1296
max_rate = 2048                     # 2 Mbps

[network.ntp]
enable = 0                          # ⭐ TẮT NTP
refresh_time_s = 60                 # (không dùng)
ntp_server = 119.28.183.184         # (không dùng)
```

#### Các Setting Quan Trọng

| Field | Giá Trị | Tại Sao |
|-------|---------|---------|
| `file_format` | `ts` | MPEG-TS chống corrupt tốt hơn MP4 khi mất điện |
| `file_duration_s` | `180` | File nhỏ (3 phút) dễ quản lý, ít mất data khi corrupt |
| `network.ntp.enable` | `0` | **CRITICAL** - Tắt NTP để timezone không bị reset |

---

### 📄 `scripts/build_firmware.sh` - Build Script

#### Chức Năng
1. **Compile Binary**
   ```bash
   arm-rockchip830-linux-uclibcgnueabihf-gcc -static -O2 \
       -o luckfox_web_config src/web_config.c
   ```
   - Cross-compile cho ARM 32-bit
   - Static linking (không cần shared libs)
   - Verify architecture: ARM EABI5

2. **Clean Rootfs**
   ```bash
   sudo rm -rf "$SDK_PATH/output/out/rootfs_uclibc_rv1106"
   ```
   - Xóa rootfs cũ để tránh stale files
   - Build fresh mỗi lần

3. **Build Rootfs**
   ```bash
   ./build.sh rootfs
   ```
   - Gọi SDK build system
   - Tạo filesystem mới

4. **Install Files**
   - Copy binary → `/oem/usr/bin/luckfox_web_config`
   - Copy init script → `/etc/init.d/S99luckfox_video`
   - Copy config → `/userdata/rkipc.ini`
   - Copy TZ file → `/etc/TZ`
   - Copy timezone.sh → `/etc/profile.d/timezone.sh`
   - Copy template → `/oem/etc/rkipc.ini.template`

5. **Package Firmware**
   ```bash
   ./build.sh firmware
   ```
   - Tạo file `update.img`
   - Rename với timestamp + MD5: `update_v2.1_20251125_011143_78fc2.img`

#### Verify Steps
- Check binary MD5
- Check file sizes
- Verify ARM architecture

---

## 4. Luồng Hoạt Động

### 🔄 Boot Sequence
```
1. Kernel boots
   ↓
2. Init process (PID 1)
   ↓
3. Run /etc/init.d/S* scripts (S00 → S99)
   ↓
4. S99luckfox_video executes
   ↓
5. Source /etc/profile.d/timezone.sh
   ↓
6. TZ='ICT-7' exported
   ↓
7. rkipc starts (inherits TZ)
   ↓
8. luckfox_web_config starts
   ↓
9. System ready
```

### 📹 Recording Flow
```
1. rkipc reads /userdata/rkipc.ini
   ↓
2. Check storage.0.enable = 1
   ↓
3. Create /mnt/sdcard/recordings/
   ↓
4. Start video capture from camera
   ↓
5. Encode H.265
   ↓
6. Write to .ts file (180s segments)
   ↓
7. Rotate files when full
   ↓
8. Delete old files when storage < 1GB
```

### 🌐 Web UI Flow
```
1. Browser → http://172.32.0.93:8080
   ↓
2. luckfox_web_config receives request
   ↓
3. Check Authorization header
   ↓
4. If GET /: send_html()
   ↓
5. If GET /api/status: send_status()
   ↓
6. If POST /api/config: handle_config_update()
   ↓
7. JavaScript auto-refresh every 5s
```

---

## 5. Các Vấn Đề Đã Giải Quyết

### ❌ Problem 1: "RTSP Stream OFF, Recording OFF" (False Alarm)
**Nguyên nhân:**
- `pgrep rkipc` match process name không chính xác
- Process running nhưng chưa bind port 554

**Giải pháp:**
- Check port 554 listening bằng `netstat` hoặc `ss`
- Fallback to pgrep nếu netstat fail

**Code:**
```c
int get_rtsp_status() {
    // Primary: check port 554
    int ret = system("netstat -ln | grep -q ':554 '");
    if (ret == 0) return 1;
    
    // Fallback: check process
    ret = system("pgrep rkipc > /dev/null");
    return (ret == 0) ? 1 : 0;
}
```

---

### ❌ Problem 2: Recording Tự Tắt Sau 1 Lát
**Nguyên nhân:**
- Init script không restart rkipc sau boot
- Config `storage.0.enable = 0` mặc định

**Giải pháp:**
- Init script force `killall rkipc` rồi restart
- Sed command set `enable = 1` mỗi lần boot

**Code:**
```bash
# Always restart to ensure config applied
killall rkipc 2>/dev/null || true
sleep 2
export TZ='ICT-7'
cd /oem && /oem/usr/bin/rkipc -a /oem/usr/share/iqfiles &
```

---

### ❌ Problem 3: Memory Hiển Thị Sai (99% Full)
**Nguyên nhân:**
- `sysinfo()` chỉ có `freeram` và `bufferram`
- Tính `used = total - free` → không trừ cache
- Linux cache disk → RAM usage cao (nhưng available vẫn nhiều)

**Giải pháp:**
- Đọc từ `/proc/meminfo`
- Dùng `MemAvailable` (kernel tính sẵn, accurate nhất)
- `used = MemTotal - MemAvailable`

**Code:**
```c
FILE *fp = fopen("/proc/meminfo", "r");
fscanf(fp, "MemTotal: %lu kB", &total_kb);
// ... scan for MemAvailable
unsigned long used_mb = (total_kb - available_kb) / 1024;
```

**Kết quả:**
- Trước: "52M / 54M (96%)" ❌
- Sau: "22M / 54M (40%)" ✅

---

### ❌ Problem 4: Timezone Đúng 1 Phút Rồi Sai
**Nguyên nhân:**
- rkipc có **NTP client** bật sẵn
- `network.ntp.enable = 1`, `refresh_time_s = 60`
- Mỗi 60 giây, NTP sync time về UTC (không có timezone offset)
- System time bị override → mất timezone

**Triệu chứng:**
- Boot: giờ đúng (từ init script TZ='ICT-7')
- Sau 60s: giờ sai (NTP reset về UTC)
- Sau 120s: giờ sai tiếp (NTP sync lại)

**Giải pháp:**
1. **Disable NTP** trong config:
   ```bash
   sed -i '/^\[network\.ntp\]/,/^\[/ s/^enable.*=.*/enable = 0/'
   ```

2. **Không dùng NTP vì:**
   - Thiết bị không có RTC (Real-Time Clock)
   - Reboot = mất thời gian hiện tại
   - NTP sync về UTC, không respect TZ environment
   - Camera không cần time chính xác tuyệt đối

3. **Alternative:**
   - Dùng timestamp từ RTSP client (nếu cần)
   - Hoặc patch rkipc để NTP respect TZ (phức tạp)

---

### ❌ Problem 5: Không Ghi File .ts
**Nguyên nhân:**
- Overlay `userdata/rkipc.ini` có `file_format = ts`
- Nhưng **userdata partition KHÔNG được flash** khi update firmware
- Rockchip firmware update chỉ flash boot/rootfs/oem, không touch userdata (để bảo vệ dữ liệu)
- Template `/oem/etc/rkipc.ini.template` vẫn là `mp4`

**Giải pháp:**
1. **Init script tự động convert:**
   ```bash
   sed -i 's/^file_format.*=.*$/file_format = ts/' /userdata/rkipc.ini
   ```

2. **Fix template** để không bị revert:
   ```bash
   sed -i 's/^file_format.*=.*$/file_format = ts/' /oem/etc/rkipc.ini.template
   ```

3. **Update overlay:**
   - `overlay/userdata/rkipc.ini`: `file_format = ts`
   - `overlay/oem/etc/rkipc.ini.template`: `file_format = ts`

**Tại sao dùng .ts thay vì .mp4?**
- MPEG-TS có recovery tốt hơn khi corrupt
- MP4 cần write footer (moov atom) cuối file
- Nếu mất điện → MP4 corrupt toàn bộ, TS chỉ mất segment cuối

---

### ❌ Problem 6: /etc/TZ Không Tự Động Load
**Nguyên nhân:**
- File `/etc/TZ` chỉ là text file
- KHÔNG có cơ chế tự động export vào environment
- Process mới không inherit TZ

**Giải pháp:**
- Tạo `/etc/profile.d/timezone.sh`:
  ```bash
  #!/bin/sh
  if [ -f /etc/TZ ]; then
      export TZ=$(cat /etc/TZ)
  fi
  ```

- Buildroot auto-source tất cả `/etc/profile.d/*.sh` khi login
- Mọi shell/process mới đều có TZ environment

---

## 6. API Reference

### GET /api/status
**Response:**
```json
{
  "uptime": "2d 3h 45m",
  "memory": "22M / 54M (40%)",
  "storage": "12.5G / 119.1G",
  "recording_count": 245,
  "time": "14:32:15",
  "rtsp_status": 1,
  "recording_status": 1,
  "sd_status": 2
}
```

### GET /api/config
**Response:**
```json
{
  "file_duration": 180,
  "width": 2304,
  "height": 1296,
  "max_rate": 2048,
  "output_data_type": "H.265"
}
```

### POST /api/config
**Request:**
```json
{
  "file_duration": 300,
  "width": 1920,
  "height": 1080,
  "max_rate": 4096
}
```

**Response:**
```json
{
  "success": true,
  "message": "Configuration updated and rkipc restarted"
}
```

---

## 7. Troubleshooting Guide

### Vấn Đề: Web UI Không Load
```bash
# 1. Check process
ssh root@172.32.0.93 "ps | grep luckfox_web_config"

# 2. Check port
ssh root@172.32.0.93 "netstat -tuln | grep 8080"

# 3. Manual start để xem lỗi
ssh root@172.32.0.93
killall luckfox_web_config
/oem/usr/bin/luckfox_web_config
```

### Vấn Đề: Timezone Vẫn Sai
```bash
# 1. Check NTP disabled
grep -A 3 '\[network.ntp\]' /userdata/rkipc.ini
# Phải thấy: enable = 0

# 2. Check TZ file
cat /etc/TZ
# Phải thấy: ICT-7

# 3. Check TZ environment
echo $TZ
# Phải thấy: ICT-7
```

### Vấn Đề: Không Ghi File .ts
```bash
# 1. Check config
grep file_format /userdata/rkipc.ini
# Phải thấy: file_format = ts (không phải mp4)

# 2. Check storage enabled
grep -A 10 '\[storage.0\]' /userdata/rkipc.ini | grep enable
# Phải thấy: enable = 1

# 3. Check files
ls -lh /mnt/sdcard/recordings/ | tail -5
```

---

## 8. Development Workflow

### Fast Iteration (Chỉ Update Binary)
```bash
# 1. Edit source
vim src/web_config.c

# 2. Build (trong SDK container hoặc với toolchain)
cd /home/becube/luckfox-pico-test
./scripts/build_firmware.sh
# Ctrl+C sau khi compile xong, trước bước build rootfs

# 3. Upload
scp luckfox_web_config root@172.32.0.93:/oem/usr/bin/

# 4. Restart
ssh root@172.32.0.93 "killall luckfox_web_config; /oem/usr/bin/luckfox_web_config &"

# 5. Test
curl http://172.32.0.93:8080/api/status
```

### Full Rebuild (Update Firmware)
```bash
# 1. Edit code/configs
vim src/web_config.c
vim overlay/etc/init.d/S99luckfox_video

# 2. Build firmware
./scripts/build_firmware.sh

# 3. Flash
# (Dùng RKDevTool trên Windows)

# 4. Verify
ssh root@172.32.0.93 "md5sum /oem/usr/bin/luckfox_web_config"
```

---

## 9. Performance Metrics

### Memory Usage
- luckfox_web_config: ~2MB RSS
- rkipc: ~15MB RSS
- Total system: ~25MB / 54MB (46%)

### CPU Usage
- Idle: 5-10%
- Recording: 30-40%
- Streaming: 20-30%

### Network
- RTSP: ~2 Mbps (configurable)
- Web UI: <100 KB/request

---

## 10. Security Considerations

### Authentication
- Basic Auth: `admin:luckfox`
- **⚠️ CẢNH BÁO:** Mật khẩu hardcoded, không dùng cho production
- **Khuyến nghị:** Change password trong code hoặc dùng HTTPS

### File Permissions
- `/userdata/rkipc.ini`: 644 (readable by all, writable by root)
- `/oem/usr/bin/luckfox_web_config`: 755 (executable)
- `/etc/init.d/S99luckfox_video`: 755 (executable)

### Network Exposure
- Port 8080: HTTP (không mã hóa)
- Port 554: RTSP (không auth)
- **Khuyến nghị:** Chỉ dùng trong mạng LAN, không expose ra Internet

---

## 11. Future Improvements

### Planned Features
- [ ] HTTPS support (SSL/TLS)
- [ ] Stronger authentication (JWT tokens)
- [ ] Multi-user support
- [ ] Video preview trong Web UI
- [ ] Config backup/restore
- [ ] OTA firmware update

### Known Limitations
- Không hỗ trợ WiFi config qua Web UI
- Không có motion detection settings
- Camera settings (brightness, contrast) phải edit file

---

## 12. Network Configuration

### LAN Port (eth0)
Mặc định, firmware được cấu hình để sử dụng **DHCP** trên cổng LAN vật lý.
Khi cắm dây mạng vào router, board sẽ tự nhận IP.

Để sử dụng **Static IP** (nếu cắm trực tiếp vào PC), sửa file `/etc/network/interfaces`:

```bash
auto eth0
iface eth0 inet static
address 192.168.1.100
netmask 255.255.255.0
gateway 192.168.1.1
```

### USB RNDIS (usb0)
Cổng USB ảo vẫn được giữ làm backup với IP mặc định `172.32.0.93`.
Nếu không kết nối được qua LAN, bạn vẫn có thể dùng USB để debug.

---

## 13. References

### Documentation
- [Luckfox Pico SDK](https://github.com/LuckfoxTECH/luckfox-pico)
- [RV1106 Datasheet](https://www.rock-chips.com/a/en/products/RV11_Series/2022/0214/1524.html)
- [Buildroot Manual](https://buildroot.org/downloads/manual/manual.html)

### Related Projects
- [rkipc](https://github.com/rockchip-linux/rkmedia) - Rockchip IPC framework
- [Live555](http://www.live555.com/) - RTSP library

---

**📅 Last Updated:** November 25, 2025
**✍️ Author:** BeCube Team
**📧 Contact:** becube@luckfox.com
**🔗 Repository:** https://github.com/cuminhbecube/camera-rv1106
