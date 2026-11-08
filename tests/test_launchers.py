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
        for launcher in ['start-ground.sh', 'start-air.sh', 'start-demo.sh']:
            with self.subTest(launcher=launcher), tempfile.TemporaryDirectory() as cwd:
                result = subprocess.run(['bash', str(root / launcher), '--help'],
                                        cwd=cwd, env=env, capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('--port', result.stdout)
                self.assertIn('--can-channel', result.stdout)


if __name__ == '__main__':
    unittest.main()
