#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$task_root/start-console.sh" --help >/dev/null
task_python=${CAN_CONSOLE_PYTHON:-"$task_root/.console-venv/bin/python"}
if ! "$task_python" -c 'import tkinter' >/dev/null 2>&1; then
    echo 'Нужен Tk для Python. Ubuntu/Debian: sudo apt install python3-tk' >&2
    exit 1
fi
exec "$task_python" "$task_root/gcs_panel.py" "$@"
