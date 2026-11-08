import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
import time
import queue
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class ConsoleTests(unittest.TestCase):
    def test_interrupted_demo_cannot_exit_successfully(self):
        from tools.can_console import main
        with patch('tools.can_console.CanModem') as modem, patch('builtins.print'):
            modem.return_value.__enter__.side_effect = KeyboardInterrupt
            with self.assertRaises(SystemExit) as result:
                main(['--port', 'dummy', '--demo'])
            self.assertEqual(result.exception.code, 130)

    def test_demo_rejects_old_frames_and_checks_current_round_trip(self):
        from tools.can_console import main
        from tools.can_client import CanFrame

        class Device:
            def __init__(self, stale):
                self.queue = queue.Queue()
                self.queue.put(CanFrame(0x123, bytes.fromhex(stale)))
                self.sent = []
                self.peer = None

            def read(self, timeout):
                try: return self.queue.get(timeout=timeout)
                except queue.Empty: return None

            def send(self, frame):
                self.sent.append(frame)
                if self.peer: self.peer.queue.put(frame)

            def close(self): pass

        for working in (False, True):
            ground = Device('0807060504030201')
            air = Device('0102030405060708')
            if working:
                ground.peer, air.peer = air, ground
            with self.subTest(working=working), patch('tools.can_console.CanModem') as modem, \
                    patch('tools.can_console.Canable', return_value=air), patch('builtins.print') as output:
                modem.return_value.__enter__.return_value = ground
                args = ['--port', 'dummy', '--demo', '--demo-timeout', '.1']
                if working:
                    main(args)
                    self.assertEqual(len(ground.sent[0].data), 8)
                    self.assertNotEqual(ground.sent[0].data, bytes.fromhex('0102030405060708'))
                else:
                    with self.assertRaises(SystemExit) as result:
                        main(args)
                    self.assertEqual(result.exception.code, 1)
                    self.assertFalse(any('ТЕСТ ПРОШЁЛ' in str(call) for call in output.call_args_list))

    def test_windows_port_detection_refuses_ambiguous_boards(self):
        from tools.can_console import ground_port
        ground = SimpleNamespace(device='COM5', vid=0x1A86, pid=0x7523, description='CH340')
        other = SimpleNamespace(device='COM7', vid=0x1A86, pid=0x7523, description='CH340')
        with patch('serial.tools.list_ports.comports', return_value=[ground]):
            self.assertEqual(ground_port(None), 'COM5')
        with patch('serial.tools.list_ports.comports', return_value=[ground, other]):
            with self.assertRaisesRegex(RuntimeError, 'COM5.*COM7'):
                ground_port(None)
            self.assertEqual(ground_port('COM7'), 'COM7')

    def test_two_pc_roles_open_only_local_hardware(self):
        from tools.can_console import main
        for role, command in [('ground', 'send 00010203'), ('air', 'send 04050607')]:
            with self.subTest(role=role), \
                    patch('tools.can_console.CanModem') as modem_type, \
                    patch('tools.can_console.Canable') as canable_type, \
                    patch('builtins.input', side_effect=[command, 'exit']), \
                    patch('builtins.print'):
                modem = modem_type.return_value.__enter__.return_value
                canable = canable_type.return_value
                for device in (modem, canable):
                    device.read.side_effect = lambda timeout: time.sleep(.01)
                main(['--role', role, '--port', 'COM5'])
                if role == 'ground':
                    canable_type.assert_not_called()
                    self.assertEqual(modem.send.call_args.args[0].data, bytes(range(4)))
                    modem_type.return_value.__exit__.assert_called_once()
                else:
                    modem_type.assert_not_called()
                    self.assertEqual(canable.send.call_args.args[0].data, bytes(range(4, 8)))
                    canable.close.assert_called_once()

    def test_console_payload_and_received_output(self):
        self.assertIsNotNone(importlib.util.find_spec('tools.can_console'), 'Interactive CAN console is missing')
        from tools.can_console import payload_frame, frame_text
        frame = payload_frame('0102030405060708', 0x123, False)
        self.assertEqual(frame.data, bytes(range(1, 9)))
        self.assertEqual(frame_text('БОРТ ПОЛУЧИЛ', frame),
                         'БОРТ ПОЛУЧИЛ | ID=123 | DLC=8 | DATA=0102030405060708')
        self.assertEqual(payload_frame('00 01 02 03', 0x456, False).data, bytes(range(4)))
        for data in ['HELLO', '123', '00' * 9, '`00010203`']:
            with self.subTest(data=data), self.assertRaises(ValueError):
                payload_frame(data, 0x123, False)


if __name__ == '__main__':
    unittest.main()
