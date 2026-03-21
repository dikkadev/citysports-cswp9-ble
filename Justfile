# CitySports CS-WP9 Treadmill Controller
# ESP32-H2 Zigbee + BLE

default:
    @just --list

# ============================================================================
# Configuration
# ============================================================================

# Serial port (Windows COM port) — update for your device
port := "COM9"

# ESP-IDF
idf_path := env_var_or_default("IDF_PATH", home_directory() + "/esp/esp-idf")
idf_version := "v5.3.3"

# Paths
firmware_dir := "firmware"
build_dir := firmware_dir + "/build"
bin_name := "treadmill"

# Windows temp for flashing
win_temp := `cmd.exe /c "echo %TEMP%" 2>/dev/null | tr -d '\r'`

# ============================================================================
# Setup (run once)
# ============================================================================

# Install ESP-IDF and dependencies
setup-idf:
    #!/usr/bin/env bash
    set -euo pipefail

    echo "=== Installing ESP-IDF prerequisites ==="
    sudo apt-get update
    sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
        cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

    echo "=== Cloning ESP-IDF {{idf_version}} ==="
    mkdir -p ~/esp
    cd ~/esp
    if [ -d "esp-idf" ]; then
        echo "esp-idf already exists, updating..."
        cd esp-idf
        git fetch
        git checkout {{idf_version}}
        git submodule sync --recursive
        git submodule update --init --recursive --force
    else
        git clone https://github.com/espressif/esp-idf.git
        cd esp-idf
        git checkout {{idf_version}}
        git submodule update --init --recursive --force
    fi

    echo "=== Running ESP-IDF install script ==="
    ./install.sh esp32h2

    echo ""
    echo "=== Setup complete! ==="
    echo "Add this to your ~/.bashrc or run before each session:"
    echo "  source ~/esp/esp-idf/export.sh"

# ============================================================================
# Build
# ============================================================================

[private]
_idf-env:
    #!/usr/bin/env bash
    if [ -z "${IDF_PATH:-}" ]; then
        source ~/esp/esp-idf/export.sh > /dev/null 2>&1
    fi

# Set target to ESP32-H2 (run once after init)
set-target: _idf-env
    #!/usr/bin/env bash
    source ~/esp/esp-idf/export.sh
    cd {{firmware_dir}}
    idf.py set-target esp32h2

# Build the firmware
build: _idf-env
    #!/usr/bin/env bash
    source ~/esp/esp-idf/export.sh
    cd {{firmware_dir}}
    idf.py build

# Clean build artifacts
clean: _idf-env
    #!/usr/bin/env bash
    source ~/esp/esp-idf/export.sh
    cd {{firmware_dir}}
    idf.py fullclean

# Open menuconfig
menuconfig: _idf-env
    #!/usr/bin/env bash
    source ~/esp/esp-idf/export.sh
    cd {{firmware_dir}}
    idf.py menuconfig

# ============================================================================
# Flash (via Windows — COM ports)
# ============================================================================

[private]
_copy-to-windows:
    #!/usr/bin/env bash
    win_temp=$(wslpath "{{win_temp}}")/{{bin_name}}
    mkdir -p "$win_temp"
    cp {{build_dir}}/{{bin_name}}.bin "$win_temp/" 2>/dev/null || cp {{build_dir}}/*.bin "$win_temp/"
    cp {{build_dir}}/bootloader/bootloader.bin "$win_temp/"
    cp {{build_dir}}/partition_table/partition-table.bin "$win_temp/"
    echo "Copied to: $win_temp"

# Flash firmware to device (builds first)
flash: build _copy-to-windows
    #!/usr/bin/env bash
    win_temp_path="{{win_temp}}\\{{bin_name}}"

    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "python -m esptool --chip esp32h2 --port {{port}} --baud 460800 write_flash --force \
         --flash-mode dio --flash-freq 48m --flash-size 4MB \
         0x0 '$win_temp_path\\bootloader.bin' \
         0x8000 '$win_temp_path\\partition-table.bin' \
         0x10000 '$win_temp_path\\{{bin_name}}.bin'"

# Flash without rebuilding
flash-only: _copy-to-windows
    #!/usr/bin/env bash
    win_temp_path="{{win_temp}}\\{{bin_name}}"

    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "python -m esptool --chip esp32h2 --port {{port}} --baud 460800 write_flash --force \
         --flash-mode dio --flash-freq 48m --flash-size 4MB \
         0x0 '$win_temp_path\\bootloader.bin' \
         0x8000 '$win_temp_path\\partition-table.bin' \
         0x10000 '$win_temp_path\\{{bin_name}}.bin'"

# Erase flash completely
erase:
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "python -m esptool --chip esp32h2 --port {{port}} erase_flash"

# ============================================================================
# Monitor
# ============================================================================

# Monitor serial output (via Windows)
monitor:
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "python -m serial.tools.miniterm {{port}} 115200"

# Build, flash, and monitor in one go
run: flash monitor

# ============================================================================
# Utilities
# ============================================================================

# Check setup
check:
    #!/usr/bin/env bash
    echo "=== ESP-IDF ==="
    if [ -d "{{idf_path}}" ]; then
        echo "OK: ESP-IDF at {{idf_path}}"
        source {{idf_path}}/export.sh 2>/dev/null && echo "OK: environment loads" || echo "FAIL: export.sh"
    else
        echo "MISSING: run 'just setup-idf'"
    fi

    echo ""
    echo "=== Project ==="
    [ -f "{{firmware_dir}}/CMakeLists.txt" ] && echo "OK: CMakeLists.txt" || echo "MISSING: CMakeLists.txt"
    [ -f "{{firmware_dir}}/main/main.c" ] && echo "OK: main.c" || echo "MISSING: main.c"

    echo ""
    echo "=== Windows tools ==="
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command "python --version" 2>/dev/null && echo "OK: Windows Python" || echo "MISSING: Python"
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command "python -m esptool version" 2>/dev/null && echo "OK: esptool" || echo "MISSING: esptool"

# Show connected serial ports (Windows)
ports:
    /mnt/c/Program\ Files/PowerShell/7/pwsh.exe -NoLogo -NoProfile -Command \
        "Get-WmiObject Win32_SerialPort | Select-Object DeviceID, Caption, Description | Format-Table -AutoSize"

# Factory reset (erase + reflash)
factory-reset: erase flash
