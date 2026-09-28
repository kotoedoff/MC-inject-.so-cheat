#!/usr/bin/env bash
# Remote inject: attach gdb to a RUNNING Minecraft and dlopen libvoltage.so
# without restarting the game. The library constructor does the rest (JVM poll,
# JVMTI gate, GL hook). Usage: ./scripts/inject_remote.sh [pid|nick]
set -euo pipefail

SO="$(cd "$(dirname "$0")/.." && pwd)/build/libvoltage.so"
[[ -f "$SO" ]] || { echo "no $SO, run ./scripts/build.sh first" >&2; exit 1; }

PID="${1:-}"
if [[ -z "$PID" ]]; then
  PID=$(pgrep -f "net.minecraft.client.main.Main" | head -1 || true)
fi
[[ -n "$PID" ]] || { echo "Minecraft process not found (launch it first)" >&2; exit 1; }
ps -p "$PID" >/dev/null || { echo "pid $PID not running" >&2; exit 1; }

echo "[*] injecting $SO into pid $PID ($(cat /proc/$PID/comm))"
echo "[!] game will freeze for ~1-2 seconds while gdb attaches"

gdb -p "$PID" -batch \
  -ex "set pagination off" \
  -ex "call (void*)dlopen(\"$SO\", 2)" \
  -ex "detach" \
  -ex "quit" 2>&1 | grep -v "^\[" | grep -v "Reading symbols" || true

echo "[*] done. tail -f /tmp/voltage.log"
