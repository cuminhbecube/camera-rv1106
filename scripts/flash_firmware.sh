#!/usr/bin/env bash
# Flash firmware to Luckfox Pico Pro via USB OTG or SD card

set -euo pipefail

SDK_PATH="${LUCKFOX_SDK:-$HOME/luckfox-pico}"
FIRMWARE_IMG="$SDK_PATH/output/image/update.img"
METHOD="${1:-usb}"  # usb or sdcard

echo "=== Luckfox Pico Pro Firmware Flash Tool ==="

if [[ ! -f "$FIRMWARE_IMG" ]]; then
    echo "ERROR: Firmware not found at $FIRMWARE_IMG"
    echo "Run ./scripts/build_firmware.sh first"
    exit 1
fi

echo "Firmware: $FIRMWARE_IMG"
echo "Size: $(du -h "$FIRMWARE_IMG" | cut -f1)"
echo "Method: $METHOD"
echo

# Check dependencies
check_tool() {
    if ! command -v "$1" &> /dev/null; then
        echo "ERROR: $1 not found. Install with:"
        echo "  sudo apt-get install $2"
        exit 1
    fi
}

if [[ "$METHOD" == "usb" ]]; then
    echo "=== USB OTG Flashing (Rockchip Maskrom Mode) ==="
    
    # Check rkdeveloptool
    if ! command -v rkdeveloptool &> /dev/null; then
        echo "Installing rkdeveloptool..."
        cd /tmp
        git clone https://github.com/rockchip-linux/rkdeveloptool.git
        cd rkdeveloptool
        autoreconf -i
        ./configure
        make
        sudo make install
        cd - > /dev/null
    fi
    
    echo ""
    echo "Put board in Maskrom mode:"
    echo "  1. Power off board"
    echo "  2. Press and hold BOOT button"
    echo "  3. Connect USB Type-C cable to PC"
    echo "  4. Release BOOT button"
    echo "  5. Run: lsusb | grep 2207"
    echo ""
    read -p "Press ENTER when ready..."
    
    # Check device
    if ! lsusb | grep -q "2207"; then
        echo "ERROR: Rockchip device not detected"
        echo "Check cable and Maskrom mode"
        exit 1
    fi
    
    echo "Rockchip device detected!"
    echo "Flashing firmware..."
    
    sudo rkdeveloptool db "$SDK_PATH/tools/linux/Linux_Pack_Firmware/rockdev/MiniLoaderAll.bin"
    sudo rkdeveloptool wl 0 "$FIRMWARE_IMG"
    sudo rkdeveloptool rd
    
    echo ""
    echo "✓ Firmware flashed successfully!"
    echo "Board will reboot automatically"
    
elif [[ "$METHOD" == "sdcard" ]]; then
    echo "=== SD Card Flashing ==="
    
    check_tool "lsblk" "util-linux"
    
    echo "Available devices:"
    lsblk -d -o NAME,SIZE,TYPE,MOUNTPOINT
    echo ""
    read -p "Enter SD card device (e.g., sdb, mmcblk0): " DEVICE
    
    DEVICE="/dev/$DEVICE"
    
    if [[ ! -b "$DEVICE" ]]; then
        echo "ERROR: $DEVICE is not a block device"
        exit 1
    fi
    
    # Safety check
    echo ""
    echo "⚠️  WARNING: This will ERASE all data on $DEVICE"
    lsblk "$DEVICE"
    echo ""
    read -p "Type 'YES' to confirm: " CONFIRM
    
    if [[ "$CONFIRM" != "YES" ]]; then
        echo "Cancelled"
        exit 1
    fi
    
    # Unmount if mounted
    sudo umount "${DEVICE}"* 2>/dev/null || true
    
    echo ""
    echo "Writing firmware to $DEVICE..."
    sudo dd if="$FIRMWARE_IMG" of="$DEVICE" bs=1M status=progress conv=fsync
    sync
    
    echo ""
    echo "✓ SD card flashing complete!"
    echo "1. Safely remove SD card"
    echo "2. Insert into Luckfox Pico Pro"
    echo "3. Power on board"
    
else
    echo "ERROR: Unknown method '$METHOD'"
    echo "Usage: $0 [usb|sdcard]"
    exit 1
fi

echo ""
echo "After boot:"
echo "  - Serial console: 115200 8N1"
echo "  - Default login: root (no password)"
echo "  - Video stream: rtsp://BOARD_IP:8554/live"
echo "  - Recordings: /mnt/sd/recordings/"
