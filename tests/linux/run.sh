#!/usr/bin/env bash
set -euo pipefail
cp /source/*.sh /source/requirements*.txt /source/gcs_panel.py /tmp/can-demo/
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
