#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$task_root"
exec python3 -m tools.socketcan --channel "${1:-can0}" --bitrate "${2:-500000}"
