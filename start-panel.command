#!/bin/bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$task_root"
if [ -x "$task_root/.venv/bin/python" ]; then
    task_python="$task_root/.venv/bin/python"
elif [ -x "$task_root/../.tools/can-venv/bin/python" ]; then
    task_python="$task_root/../.tools/can-venv/bin/python"
else
    task_python=python3
fi
exec "$task_python" "$task_root/gcs_panel.py"
