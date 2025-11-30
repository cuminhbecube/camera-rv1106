#!/bin/bash
#
# Luckfox Pico Pro - ALL-IN-ONE Firmware Builder
# Compile binary + Build firmware + Auto-naming
#

set -e

VERSION="2.1"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SDK_PATH="/home/becube/luckfox-pico"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN="$SDK_PATH/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"
CC="$TOOLCHAIN/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc"

echo "╔═══════════════════════════════════════════════════╗"
echo "║   FIRMWARE v${VERSION} - ALL-IN-ONE BUILDER      ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""

# =============================================================================
# STEP 1: COMPILE BINARY
# =============================================================================
echo "╔═══════════════════════════════════════════════════╗"
echo "║  [1/5] COMPILE luckfox_web_config for ARM        ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""

cd "$PROJECT_ROOT"

# Remove old binary
if [[ -f "luckfox_web_config" ]]; then
    rm -f luckfox_web_config
    echo "Removed old binary"
fi

# Check source
if [[ ! -f "src/web_config.c" ]]; then
    echo "❌ ERROR: src/web_config.c not found!"
    exit 1
fi

# Check toolchain
if [[ ! -f "$CC" ]]; then
    echo "❌ ERROR: ARM toolchain not found at $CC"
    exit 1
fi

# Compile
echo "Compiling with ARM toolchain..."
$CC -o luckfox_web_config src/web_config.c -Wall -O2 -lpthread

if [[ ! -f "luckfox_web_config" ]]; then
    echo "❌ Compilation failed!"
    exit 1
fi

# Verify architecture
ARCH_CHECK=$(file luckfox_web_config | grep "ARM" || true)
if [[ -z "$ARCH_CHECK" ]]; then
    echo "❌ ERROR: Binary is NOT ARM architecture!"
    file luckfox_web_config
    exit 1
fi

BIT_CHECK=$(file luckfox_web_config | grep "32-bit" || true)
if [[ -z "$BIT_CHECK" ]]; then
    echo "❌ ERROR: Binary is NOT 32-bit!"
    file luckfox_web_config
    exit 1
fi

BINARY_MD5=$(md5sum luckfox_web_config | awk '{print $1}')
BINARY_SIZE=$(ls -lh luckfox_web_config | awk '{print $5}')

echo ""
echo "✅ Binary compiled successfully!"
echo "   Architecture: ARM 32-bit EABI5"
echo "   Size: $BINARY_SIZE"
echo "   MD5: $BINARY_MD5"
echo ""

# =============================================================================
# STEP 2: CLEAN ROOTFS & BUILD CACHE
# =============================================================================
echo "╔═══════════════════════════════════════════════════╗"
echo "║  [2/5] CLEAN PREVIOUS ROOTFS & CACHE              ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""

# Clean rootfs output
if [[ -d "$SDK_PATH/output/out/rootfs_uclibc_rv1106" ]]; then
    echo "Removing old rootfs (requires sudo)..."
    sudo rm -rf "$SDK_PATH/output/out/rootfs_uclibc_rv1106"
    echo "✅ Rootfs cleaned"
fi

# Clean Buildroot build artifacts to force fresh build
echo "Cleaning Buildroot cache..."
if [[ -d "$SDK_PATH/sysdrv/out/rootfs_uclibc_rv1106" ]]; then
    sudo rm -rf "$SDK_PATH/sysdrv/out/rootfs_uclibc_rv1106"
fi

# Clean old images
if [[ -d "$SDK_PATH/output/image" ]]; then
    rm -f "$SDK_PATH/output/image/oem.img"
    rm -f "$SDK_PATH/output/image/rootfs.img"
    echo "✅ Old images cleaned"
fi

echo "✅ All caches cleaned"
echo ""

# =============================================================================
# STEP 3: BUILD ROOTFS
# =============================================================================
echo ""
echo "╔═══════════════════════════════════════════════════╗"
echo "║  [3/5] BUILD ROOTFS (may take 2-5 minutes)        ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""

cd "$SDK_PATH"

# Clean PATH for Buildroot (remove Windows/WSL paths with spaces)
export PATH=$(python3 -c "import os; print(':'.join([p for p in os.environ.get('PATH', '').split(':') if '/mnt/c/' not in p and 'Program Files' not in p and ' ' not in p]))")

./build.sh rootfs

if [[ $? -ne 0 ]]; then
    echo "❌ Rootfs build failed!"
    exit 1
fi

echo ""
echo "✅ Rootfs built successfully"
echo ""

# =============================================================================
# STEP 4: INSTALL BINARY & OVERLAY
# =============================================================================
echo "╔═══════════════════════════════════════════════════╗"
echo "║  [4/5] INSTALL BINARY & OVERLAY FILES             ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""

ROOTFS="$SDK_PATH/output/out/rootfs_uclibc_rv1106"
OEM_PATH="$SDK_PATH/output/out/oem"
APP_OUT="$SDK_PATH/output/out/app_out/bin"

# Verify rootfs exists
if [[ ! -d "$ROOTFS" ]]; then
    echo "❌ ERROR: Rootfs not found at $ROOTFS"
    exit 1
fi

# Install binary to BOTH APP_OUT and OEM directly
echo "[4.1] Installing luckfox_web_config..."
mkdir -p "$APP_OUT"
mkdir -p "$OEM_PATH/usr/bin"

# Copy to APP_OUT (for SDK reference)
cp -f "$PROJECT_ROOT/luckfox_web_config" "$APP_OUT/"
chmod +x "$APP_OUT/luckfox_web_config"

# Copy DIRECTLY to OEM (to bypass SDK stripping)
cp -f "$PROJECT_ROOT/luckfox_web_config" "$OEM_PATH/usr/bin/"
chmod +x "$OEM_PATH/usr/bin/luckfox_web_config"

# Verify both copies
APP_MD5=$(md5sum "$APP_OUT/luckfox_web_config" | awk '{print $1}')
OEM_MD5=$(md5sum "$OEM_PATH/usr/bin/luckfox_web_config" | awk '{print $1}')

if [[ "$BINARY_MD5" == "$APP_MD5" ]] && [[ "$BINARY_MD5" == "$OEM_MD5" ]]; then
    echo "✅ Binary installed to APP_OUT and OEM (MD5: $BINARY_MD5)"
else
    echo "❌ MD5 mismatch!"
    echo "   Source: $BINARY_MD5"
    echo "   APP_OUT: $APP_MD5"
    echo "   OEM: $OEM_MD5"
    exit 1
fi

# Install init script
echo ""
echo "[4.2] Installing init script..."
if [[ -f "$PROJECT_ROOT/overlay/etc/init.d/S99luckfox_video" ]]; then
    mkdir -p "$ROOTFS/etc/init.d"
    cp -f "$PROJECT_ROOT/overlay/etc/init.d/S99luckfox_video" "$ROOTFS/etc/init.d/"
    chmod +x "$ROOTFS/etc/init.d/S99luckfox_video"
    echo "✅ Init script installed"
else
    echo "⚠️  WARNING: Init script not found"
fi

# Install config to USERDATA partition
echo ""
echo "[4.3] Installing rkipc.ini to USERDATA partition..."
USERDATA="$SDK_PATH/output/out/userdata"
if [[ -f "$PROJECT_ROOT/overlay/userdata/rkipc.ini" ]]; then
    mkdir -p "$USERDATA"
    cp -f "$PROJECT_ROOT/overlay/userdata/rkipc.ini" "$USERDATA/"
    echo "✅ Config installed to userdata partition"
else
    echo "⚠️  WARNING: rkipc.ini not found"
fi

# Install S00userdata_init if exists
if [[ -f "$PROJECT_ROOT/overlay/etc/init.d/S00userdata_init" ]]; then
    echo ""
    echo "[4.4] Installing S00userdata_init..."
    cp -f "$PROJECT_ROOT/overlay/etc/init.d/S00userdata_init" "$ROOTFS/etc/init.d/"
    chmod +x "$ROOTFS/etc/init.d/S00userdata_init"
    echo "✅ S00userdata_init installed"
fi

# Install template to OEM partition
if [[ -f "$PROJECT_ROOT/overlay/oem/etc/rkipc.ini.template" ]]; then
    echo ""
    echo "[4.5] Installing rkipc.ini.template to OEM..."
    mkdir -p "$OEM_PATH/etc"
    cp -f "$PROJECT_ROOT/overlay/oem/etc/rkipc.ini.template" "$OEM_PATH/etc/"
    chmod 644 "$OEM_PATH/etc/rkipc.ini.template"
    echo "✅ Template installed to OEM"
fi

echo ""
echo "✅ All files installed to rootfs and OEM"
echo ""

# =============================================================================
# STEP 5: PACKAGE FIRMWARE
# =============================================================================
echo "╔═══════════════════════════════════════════════════╗"
echo "║  [5/5] PACKAGE FIRMWARE                           ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""

cd "$SDK_PATH"
./build.sh firmware

if [[ $? -ne 0 ]]; then
    echo "❌ Packaging failed!"
    exit 1
fi

# CRITICAL FIX: SDK strips binaries - restore original luckfox_web_config
echo ""
echo "[POST-BUILD] Restoring luckfox_web_config (SDK stripped it)..."

# Restore original binary to OEM
cp -f "$PROJECT_ROOT/luckfox_web_config" "$OEM_PATH/usr/bin/"
chmod +x "$OEM_PATH/usr/bin/luckfox_web_config"

# Verify
FINAL_MD5=$(md5sum "$OEM_PATH/usr/bin/luckfox_web_config" | awk '{print $1}')
if [[ "$BINARY_MD5" == "$FINAL_MD5" ]]; then
    echo "✅ Binary restored (MD5: $BINARY_MD5)"
else
    echo "❌ CRITICAL: Binary MD5 mismatch!"
    echo "   Expected: $BINARY_MD5"
    echo "   Got: $FINAL_MD5"
    exit 1
fi

# Rebuild OEM image with correct binary
echo "[POST-BUILD] Rebuilding OEM image..."
cd "$SDK_PATH"
export PATH=$(python3 -c "import os; print(':'.join([p for p in os.environ.get('PATH', '').split(':') if '/mnt/c/' not in p and 'Program Files' not in p and ' ' not in p]))")
./build.sh updateimg
echo "✅ OEM image rebuilt with correct binary"

# Generate filename with MD5 suffix
FIRMWARE_MD5=$(md5sum "$SDK_PATH/output/image/update.img" | awk '{print $1}')
MD5_SUFFIX="${FIRMWARE_MD5: -5}"  # Last 5 characters
OUTPUT_NAME="update_v${VERSION}_${TIMESTAMP}_${MD5_SUFFIX}.img"
OUTPUT_PATH="$PROJECT_ROOT/$OUTPUT_NAME"

# Copy firmware with auto-generated name
cp "$SDK_PATH/output/image/update.img" "$OUTPUT_PATH"
FIRMWARE_SIZE=$(ls -lh "$OUTPUT_PATH" | awk '{print $5}')

# =============================================================================
# BUILD COMPLETE
# =============================================================================
echo ""
echo "╔═══════════════════════════════════════════════════╗"
echo "║         BUILD COMPLETED SUCCESSFULLY! ✅          ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""
echo "📦 FIRMWARE DETAILS:"
echo "   File:     $OUTPUT_NAME"
echo "   Location: $PROJECT_ROOT/"
echo "   Size:     $FIRMWARE_SIZE"
echo "   MD5:      $FIRMWARE_MD5"
echo ""
echo "🔧 BINARY DETAILS:"
echo "   Size:     $BINARY_SIZE"
echo "   MD5:      $BINARY_MD5"
echo "   Location: /oem/usr/bin/luckfox_web_config"
echo ""
echo "✅ FEATURES INCLUDED:"
echo "   - Read-only Web Status Monitor (port 8080, Dark Mode)"
echo "   - Basic Authentication (admin/luckfox)"
echo "   - Status + Logs API endpoints"
echo "   - Persistent SD card logging"
echo "   - Auto-start on boot"
echo ""
echo "📝 NEXT STEPS:"
echo "   1. Flash firmware:"
echo "      File: $OUTPUT_NAME"
echo ""
echo "   2. After flash, verify binary MD5:"
echo "      ssh root@172.32.0.93 \"md5sum /oem/usr/bin/luckfox_web_config\""
echo "      Expected: $BINARY_MD5"
echo ""
echo "   3. Test web UI:"
echo "      curl http://172.32.0.93:8080"
echo ""
echo "   4. Test APIs:"
echo "      curl http://172.32.0.93:8080/api/status"
echo "      curl http://172.32.0.93:8080/api/logs"
echo ""
echo "╔═══════════════════════════════════════════════════╗"
echo "║              READY TO DEPLOY! 🚀                  ║"
echo "╚═══════════════════════════════════════════════════╝"
echo ""
