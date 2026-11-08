#!/usr/bin/env bash
set -euo pipefail
cp /source/*.sh /source/requirements*.txt /source/gcs_panel.py /source/bind.py /tmp/can-demo/
cp -r /source/tools /source/tests /source/include /source/src /tmp/can-demo/
cd /tmp/can-demo
./start-ground.sh --help
./start-air.sh --help
./start-demo.sh --help
./start-ground.sh --list-ports
.console-venv/bin/python -m tools.provision --help
.console-venv/bin/python -m pip install -r requirements.txt
.console-venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v
xvfb-run -a .console-venv/bin/python - <<'PY'
import tkinter as tk
from gcs_panel import BplaGcsApp
root = tk.Tk()
app = BplaGcsApp(root)
root.after(200, root.destroy)
root.mainloop()
print('Linux GUI created and closed successfully')
PY
python3 - <<'PY'
import socket
try:
    s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    s.close()
    print('AF_CAN available; real CAN interface is required for hardware tests')
except OSError as exc:
    print('Native CAN hardware/vcan not tested: Docker host kernel lacks AF_CAN:', exc)
PY
