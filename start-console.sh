#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$task_root"
if [[ -n "${CAN_CONSOLE_PYTHON:-}" ]]; then
    task_python="$CAN_CONSOLE_PYTHON"
else
    if ! command -v python3 >/dev/null 2>&1 || ! python3 -c 'import sys; assert sys.version_info >= (3,10)' >/dev/null 2>&1; then
        echo 'Нужен Python 3.10+. Ubuntu/Debian: sudo apt install python3 python3-venv' >&2
        exit 1
    fi
    task_python="$task_root/.console-venv/bin/python"
    if [[ ! -x "$task_python" ]] || ! "$task_python" -c 'import pip' >/dev/null 2>&1; then
        if ! python3 -m venv "$task_root/.console-venv"; then
            echo 'Не удалось создать окружение. Ubuntu/Debian: sudo apt install python3-venv' >&2
            exit 1
        fi
    fi
fi
if ! "$task_python" -c 'import sys; assert sys.version_info >= (3,10)' >/dev/null 2>&1; then
    echo 'Нужен рабочий Python 3.10+.' >&2
    exit 1
fi
if ! "$task_python" -c 'import sys,importlib.metadata as m; assert m.version("pyserial")=="3.5"; assert m.version("python-can")=="4.6.1"; assert sys.platform.startswith("linux") or m.version("gs-usb")=="0.3.1"' >/dev/null 2>&1; then
    "$task_python" -m pip install --disable-pip-version-check -r requirements-console.txt
fi
exec "$task_python" -m tools.can_console --setup-can "$@"
