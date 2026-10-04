#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

echo "=== Starting VDRSD Cluster System ==="

# Build project if not built
if [ ! -f "./vdrsd_daemon" ]; then
    echo "Compiling VDRSD components..."
    make
fi

# Load kernel module if compiled and not loaded
if lsmod | grep -q "vdrsd"; then
    echo "[Kernel] Module vdrsd is already loaded."
else
    if [ -f "./kernel/vdrsd.ko" ]; then
        echo "[Kernel] Loading module vdrsd.ko..."
        sudo insmod ./kernel/vdrsd.ko || echo "[Kernel] Note: Insmod requires root."
    else
        echo "[Kernel] vdrsd.ko not built yet. Building..."
        make kernel_module || echo "[Kernel] Note: Kernel headers needed for kernel_module."
    fi
fi

# Create data directories
mkdir -p ./vdrsd_data

# Launch Daemon
echo "[Daemon] Launching VDRSD Storage Daemon..."
./vdrsd_daemon ./config/vdrsd.conf &
DAEMON_PID=$!
echo "$DAEMON_PID" > ./vdrsd.pid

echo "VDRSD started with PID: $DAEMON_PID"
