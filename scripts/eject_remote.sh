#!/usr/bin/env bash
# Remote eject: ask libvoltage to unhook (restore GLFW input mode) and dlclose
# itself from a RUNNING Minecraft. Usage: ./scripts/eject_remote.sh [pid]
set -euo pipefail

PID="${1:-}"
if [[ -z "$PID" ]]; then
  PID=$(pgrep -f "net.minecraft.client.main.Main" | head -1 || true)
fi
[[ -n "$PID" ]] || { echo "Minecraft process not found" >&2; exit 1; }

echo "[*] ejecting voltage from pid $PID"
gdb -p "$PID" -batch \
  -ex "set pagination off" \
  -ex "call (int)voltage_eject()" \
  -ex "detach" \
  -ex "quit" 2>&1 | grep -v "^\[" | grep -v "Reading symbols" || true

echo "[*] done. Check /tmp/voltage.log for the eject trace."
