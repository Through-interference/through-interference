"""Exercise the firmware's real portable codec and transport on the host."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeTests(unittest.TestCase):
    def test_bridge_recovers_radio_and_can_and_honors_fixed_channel(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / 'bridge-tests'
            build = subprocess.run([
                'c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/native/stubs'),
                '-I', str(ROOT / 'include'),
                str(ROOT / 'tests/native/bridge_recovery_test.cpp'),
                str(ROOT / 'src/common/BridgeController.cpp'),
                str(ROOT / 'src/common/RadioChannelManager.cpp'),
                str(ROOT / 'src/common/CanProtocol.cpp'),
                str(ROOT / 'src/common/ReliableEndpoint.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_binary_can_transport(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / 'can-tests'
            build = subprocess.run([
                'c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'include'),
                str(ROOT / 'tests/native/can_transport_test.cpp'),
                str(ROOT / 'src/common/CanProtocol.cpp'),
                str(ROOT / 'src/common/ReliableEndpoint.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
