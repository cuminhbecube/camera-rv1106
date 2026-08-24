#!/bin/bash
#
# Luckfox Pico Pro Max - reproducible firmware builder
# Production-safe monitor release.
#

set -euo pipefail

VERSION="${VERSION:-2.2}"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_PATH="${SDK_PATH:-/home/becube/luckfox-pico}"
TOOLCHAIN="${TOOLCHAIN:-$SDK_PATH/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf}"
CC="${CC:-$TOOLCHAIN/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc}"
CLEAN_BUILD="${CLEAN_BUILD:-1}"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

ROOTFS="$SDK_PATH/output/out/rootfs_uclibc_rv1106"
OEM_PATH="$SDK_PATH/output/out/oem"
APP_OUT="$SDK_PATH/output/out/app_out/bin"
USERDATA="$SDK_PATH/output/out/userdata"
IMAGE_DIR="$SDK_PATH/output/image"
BINARY="$PROJECT_ROOT/luckfox_web_config"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

require_file() {
    [ -f "$1" ] || die "missing file: $1"
}

clean_host_path() {
    python3 - <<'PY'
import os
parts = []
for p in os.environ.get("PATH", "").split(":"):
    if not p:
        continue
    if "/mnt/c/" in p or "Program Files" in p or " " in p:
        continue
    parts.append(p)
print(":".join(parts))
PY
}

echo "============================================================"
echo " Luckfox RV1106 firmware v$VERSION - production-safe build"
echo "============================================================"
echo "Project : $PROJECT_ROOT"
echo "SDK     : $SDK_PATH"
echo "CC      : $CC"

require_file "$PROJECT_ROOT/src/web_config.c"
require_file "$PROJECT_ROOT/overlay/etc/init.d/S00userdata_init"
require_file "$PROJECT_ROOT/overlay/etc/init.d/S99luckfox_video"
require_file "$PROJECT_ROOT/overlay/userdata/rkipc.ini"
require_file "$PROJECT_ROOT/overlay/oem/etc/rkipc.ini.template"
require_file "$CC"
require_file "$SDK_PATH/build.sh"

echo
echo "[1/5] Compile read-only status monitor"
rm -f "$BINARY"
"$CC" \
    -std=c11 -O2 -Wall -Wextra -Werror \
    -o "$BINARY" "$PROJECT_ROOT/src/web_config.c" -pthread

require_file "$BINARY"
file "$BINARY" | grep -q "ARM" || die "monitor binary is not ARM"
file "$BINARY" | grep -q "32-bit" || die "monitor binary is not 32-bit"

BINARY_MD5="$(md5sum "$BINARY" | awk '{print $1}')"
BINARY_SHA256="$(sha256sum "$BINARY" | awk '{print $1}')"
echo "Binary MD5    : $BINARY_MD5"
echo "Binary SHA256 : $BINARY_SHA256"

echo
echo "[2/5] Prepare SDK output"
if [ "$CLEAN_BUILD" = "1" ]; then
    echo "Cleaning stale rootfs/build output..."
    if [ -d "$ROOTFS" ]; then sudo rm -rf "$ROOTFS"; fi
    if [ -d "$SDK_PATH/sysdrv/out/rootfs_uclibc_rv1106" ]; then
        sudo rm -rf "$SDK_PATH/sysdrv/out/rootfs_uclibc_rv1106"
    fi
    rm -f "$IMAGE_DIR/oem.img" "$IMAGE_DIR/rootfs.img" "$IMAGE_DIR/update.img" 2>/dev/null || true
else
    echo "CLEAN_BUILD=0: keeping existing SDK output"
fi

export PATH="$(clean_host_path)"
cd "$SDK_PATH"
./build.sh rootfs
[ -d "$ROOTFS" ] || die "rootfs output not found: $ROOTFS"

echo
echo "[3/5] Install application and overlays"
mkdir -p "$APP_OUT" "$OEM_PATH/usr/bin" "$OEM_PATH/etc" "$ROOTFS/etc/init.d" "$USERDATA"

install -m 0755 "$BINARY" "$APP_OUT/luckfox_web_config"
install -m 0755 "$BINARY" "$OEM_PATH/usr/bin/luckfox_web_config"
install -m 0755 "$PROJECT_ROOT/overlay/etc/init.d/S00userdata_init" \
    "$ROOTFS/etc/init.d/S00userdata_init"
install -m 0755 "$PROJECT_ROOT/overlay/etc/init.d/S99luckfox_video" \
    "$ROOTFS/etc/init.d/S99luckfox_video"
install -m 0644 "$PROJECT_ROOT/overlay/userdata/rkipc.ini" \
    "$USERDATA/rkipc.ini"
install -m 0644 "$PROJECT_ROOT/overlay/oem/etc/rkipc.ini.template" \
    "$OEM_PATH/etc/rkipc.ini.template"

APP_MD5="$(md5sum "$APP_OUT/luckfox_web_config" | awk '{print $1}')"
OEM_MD5="$(md5sum "$OEM_PATH/usr/bin/luckfox_web_config" | awk '{print $1}')"
[ "$APP_MD5" = "$BINARY_MD5" ] || die "APP_OUT binary checksum mismatch"
[ "$OEM_MD5" = "$BINARY_MD5" ] || die "OEM binary checksum mismatch"

echo
echo "[4/5] Package firmware"
cd "$SDK_PATH"
./build.sh firmware

# Some SDK flows strip/replace app binaries during packaging. Restore the
# verified binary and rebuild update.img so the packaged OEM copy is exact.
install -m 0755 "$BINARY" "$OEM_PATH/usr/bin/luckfox_web_config"
FINAL_MD5="$(md5sum "$OEM_PATH/usr/bin/luckfox_web_config" | awk '{print $1}')"
[ "$FINAL_MD5" = "$BINARY_MD5" ] || die "post-package OEM checksum mismatch"

./build.sh updateimg
require_file "$IMAGE_DIR/update.img"

echo
echo "[5/5] Publish named image"
FIRMWARE_MD5="$(md5sum "$IMAGE_DIR/update.img" | awk '{print $1}')"
FIRMWARE_SHA256="$(sha256sum "$IMAGE_DIR/update.img" | awk '{print $1}')"
MD5_SUFFIX="${FIRMWARE_MD5: -5}"
OUTPUT_NAME="update_v${VERSION}_${TIMESTAMP}_${MD5_SUFFIX}.img"
OUTPUT_PATH="$PROJECT_ROOT/$OUTPUT_NAME"
cp "$IMAGE_DIR/update.img" "$OUTPUT_PATH"

cat <<EOF

Build complete
--------------
Firmware : $OUTPUT_PATH
MD5      : $FIRMWARE_MD5
SHA256   : $FIRMWARE_SHA256
Binary   : /oem/usr/bin/luckfox_web_config
Bin MD5  : $BINARY_MD5

Post-flash checks:
  ssh root@<ip> 'md5sum /oem/usr/bin/luckfox_web_config'
  curl -u admin:luckfox http://<ip>:8080/api/status
  curl -u admin:luckfox http://<ip>:8080/api/logs
  ffprobe -rtsp_transport tcp rtsp://<ip>:554/live/0

The web service is intentionally read-only. Camera configuration is managed
outside HTTP and the web monitor never restarts rkipc.
EOF
