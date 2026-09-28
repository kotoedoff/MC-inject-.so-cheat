#!/usr/bin/env bash
# Smoke-test: prove voltage.so loads into ANY process via LD_PRELOAD.
# Usage: ./scripts/run_test.sh [build dir, default: build]
set -euo pipefail
BUILD="${1:-build}"
SO="$(pwd)/${BUILD}/libvoltage.so"
if [[ ! -f "$SO" ]]; then echo "no $SO, run ./scripts/build.sh first" >&2; exit 1; fi
export VOLTAGE_LOG=/tmp/voltage.log
rm -f /tmp/voltage.log
echo "[*] preloading $SO into /bin/sleep 3 ..."
LD_PRELOAD="$SO" /bin/sleep 3
echo "[*] log:"
cat /tmp/voltage.log
echo "[*] OK if you see 'loaded into /bin/sleep' above"
