import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class LauncherTests(unittest.TestCase):
    def test_linux_launcher_finds_project_from_another_directory(self):
        root = Path(__file__).resolve().parents[1]
        env = dict(os.environ, CAN_CONSOLE_PYTHON=sys.executable)
        with tempfile.TemporaryDirectory() as cwd:
            result = subprocess.run(['bash', str(root / 'start-ground.sh'), '--help'],
                                    cwd=cwd, env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('--port', result.stdout)
        self.assertIn('--role', result.stdout)


if __name__ == '__main__':
    unittest.main()
