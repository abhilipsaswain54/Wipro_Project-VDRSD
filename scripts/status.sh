#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

echo "=== VDRSD Status Check ==="

if pgrep -f "vdrsd_daemon" > /dev/null; then
    echo "[Daemon] Storage Daemon: RUNNING (PID: $(pgrep -f vdrsd_daemon))"
else
    echo "[Daemon] Storage Daemon: STOPPED"
fi

if lsmod | grep -q "vdrsd"; then
    echo "[Kernel] vdrsd.ko Module: LOADED"
else
    echo "[Kernel] vdrsd.ko Module: NOT LOADED"
fi

if [ -d "./vdrsd_data" ]; then
    CHUNK_COUNT=$(ls -1 ./vdrsd_data/chunk_*.dat 2>/dev/null | wc -l)
    echo "[Storage] Saved Chunks: $CHUNK_COUNT"
else
    echo "[Storage] Directory not created yet."
fi
