#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

echo "=== Stopping VDRSD System ==="

if [ -f "./vdrsd.pid" ]; then
    PID=$(cat ./vdrsd.pid)
    echo "Stopping daemon process $PID..."
    kill -SIGTERM "$PID" 2>/dev/null || true
    rm -f ./vdrsd.pid
else
    pkill -f vdrsd_daemon || true
fi

if lsmod | grep -q "vdrsd"; then
    echo "Unloading kernel module vdrsd..."
    sudo rmmod vdrsd || echo "Note: rmmod requires root."
fi

echo "VDRSD System stopped."
