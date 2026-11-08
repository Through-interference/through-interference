"""Provision through fake serial devices, without opening any physical ports."""
from collections import deque
from contextlib import redirect_stdout, redirect_stderr
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class SerialDevice:
    def __init__(self, role):
        self.role = role
        self.port = None
        self.responses = deque()
        self.commands = []
        self.closed = False
        self.reject = None
        self.saved = False
        self.restarted = False
        self.bad_cfg = False
        self.leak_on_key = False

    def open(self):
        pass

    def reset_input_buffer(self):
        self.responses.clear()

    def write(self, data):
        command = data.decode('ascii').strip()
        self.commands.append(command)
        if self.leak_on_key and command.startswith('KEY '):
            raise RuntimeError('Backend write failed for ' + command)
        if command == self.reject:
            self.responses.extend([b'ERROR: TEST_REJECTED\r\n'])
            return len(data)
        if command == 'CAN STAT':
            self.responses.append(f'+CAN STAT: ROLE={self.role},OPEN=0,LINK=UP,PEER_READY=1,TX_PENDING=0,USB_PENDING=0,USB_RX=0,CAN_RX=0,CAN_TX=0,CAN_PENDING=0,CAN_DROP=0,CAN_FAIL=0,BUS_OFF=0,RF_TX=0,RF_RX=0,RF_TIMEOUT=0,RF_BAD=0,QUEUE_REJECT=0,UNCERTAIN=0\r\n'.encode())
        elif command == 'CFG':
            mod = 'LORA_SF9' if self.bad_cfg else 'GFSK250'
            self.responses.append(f'+CFG: PRIM=868.0, BACK=869.1, PWR=20, MOD={mod}, SLOT=0, FHSS=0\r\n'.encode())
        elif command == 'CAN DIAG':
            self.responses.append(b'+CAN DIAG: BAUD=500000,TX_PIN=4,RX_PIN=17,STATE=1,TEC=0,REC=0,HW_TX_FAIL=0,BUS_ERRORS=0,ARB_LOST=0,RX_LEVEL=1,RX_ROUTE=17,TX_ROUTE=123,TX_ENABLE=1,MODE=00,STATUS=00,BTR0=00,BTR1=00\r\n')
        elif command == 'SAVE':
            self.saved = True
        elif command == 'RST':
            self.restarted = True
        self.responses.append(b'OK\r\n')
        return len(data)

    def readline(self):
        return self.responses.popleft() if self.responses else b''

    def close(self):
        self.closed = True


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('tools.provision'), 'Two-port provisioning tool is missing')
        from tools import provision
        self.api = provision
        self.ground, self.air = SerialDevice('GROUND'), SerialDevice('AIR')
        self.output = io.StringIO()

    def run_tool(self, *extra):
        with patch('serial.Serial', side_effect=[self.ground, self.air]), \
                patch('tools.provision.time.sleep'), redirect_stdout(self.output), redirect_stderr(self.output):
            return self.api.main(['--ground-port', 'ground', '--air-port', 'air', *extra])

    def test_requires_new_key_flag_before_opening_or_changing_devices(self):
        with patch('serial.Serial') as factory, redirect_stderr(self.output):
            with self.assertRaises(SystemExit) as raised:
                self.api.main(['--ground-port', 'ground', '--air-port', 'air'])
        self.assertNotEqual(raised.exception.code, 0)
        factory.assert_not_called()

    def test_duplicate_ports_are_rejected_before_open(self):
        with patch('serial.Serial') as factory, redirect_stderr(self.output):
            with self.assertRaises(SystemExit):
                self.api.main(['--ground-port', 'same', '--air-port', 'same', '--new-key'])
        factory.assert_not_called()

    def test_wrong_roles_close_both_devices_without_key_or_save(self):
        self.air.role = 'GROUND'
        self.assertEqual(self.run_tool('--new-key'), 1)
        self.assertTrue(self.air.closed and self.ground.closed)
        self.assertFalse(any(c.startswith('KEY ') or c == 'SAVE' for d in (self.ground, self.air) for c in d.commands))

    def test_one_new_shared_key_is_saved_both_ends_and_never_printed(self):
        keys = []
        for _ in range(2):
            self.ground, self.air = SerialDevice('GROUND'), SerialDevice('AIR')
            self.assertEqual(self.run_tool('--new-key'), 0)
            ground_keys = [c[4:] for c in self.ground.commands if c.startswith('KEY ')]
            air_keys = [c[4:] for c in self.air.commands if c.startswith('KEY ')]
            self.assertEqual(ground_keys, air_keys)
            self.assertEqual(len(ground_keys), 1)
            self.assertRegex(ground_keys[0], r'^[0-9A-F]{32}$')
            self.assertNotEqual(int(ground_keys[0], 16), 0)
            keys.extend(ground_keys)
            for device in (self.ground, self.air):
                self.assertTrue(device.saved and device.restarted and device.closed)
                self.assertIn('PRIMFREQ 868.0', device.commands)
                self.assertIn('BACKFREQ 869.1', device.commands)
                self.assertIn('MOD GFSK250', device.commands)
                self.assertIn('PWR 20', device.commands)
                self.assertGreater(device.commands.index('CFG'), device.commands.index('RST'))
            self.assertIn('CANBAUD 500000', self.air.commands)
            self.assertLess(self.air.commands.index('CANBAUD 500000'), self.air.commands.index('SAVE'))
            self.assertNotIn('CANBAUD 500000', self.ground.commands)
            self.assertIn('CAN DIAG', self.air.commands)
        self.assertNotEqual(keys[0], keys[1])
        for key in keys:
            self.assertNotIn(key, self.output.getvalue())

    def test_rejected_setting_closes_both_and_cannot_claim_success(self):
        self.air.reject = 'CANBAUD 500000'
        self.assertEqual(self.run_tool('--new-key'), 1)
        self.assertTrue(self.air.closed and self.ground.closed)
        self.assertFalse(self.air.saved or self.ground.saved)

    def test_backend_key_write_error_never_exposes_key(self):
        self.air.leak_on_key = True
        self.assertEqual(self.run_tool('--new-key'), 1)
        key = next(command[4:] for command in self.air.commands if command.startswith('KEY '))
        self.assertNotIn(key, self.output.getvalue())
        self.assertTrue(self.air.closed and self.ground.closed)

    def test_second_open_failure_closes_both_serial_handles(self):
        def fail_open():
            raise OSError('port unavailable')
        self.air.open = fail_open
        self.assertEqual(self.run_tool('--new-key'), 1)
        self.assertTrue(self.air.closed and self.ground.closed)
        self.assertEqual(self.ground.commands, [])

    def test_missing_ack_is_an_error_and_does_not_return_early(self):
        device = self.api.AtPort('fake')
        device.serial = self.ground
        self.ground.reject = 'SAVE'
        def no_response(_data):
            return 6
        self.ground.write = no_response
        with patch('tools.provision.time.monotonic', side_effect=[0, 0, 5]):
            with self.assertRaises(TimeoutError):
                device.command('SAVE', timeout=4)

    def test_reloaded_wrong_profile_is_failure(self):
        self.air.bad_cfg = True
        self.assertEqual(self.run_tool('--new-key'), 1)
        self.assertTrue(self.air.closed and self.ground.closed)


if __name__ == '__main__':
    unittest.main()
