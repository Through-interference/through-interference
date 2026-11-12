"""Exercise GUI connection methods without a display or physical serial devices."""
from pathlib import Path
import queue
import sys
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    import tkinter
except ImportError:
    BplaGcsApp = None
else:
    from gcs_panel import BplaGcsApp

from tools.can_client import UsbReceiver


class Value:
    def __init__(self, value=''):
        self.value = value
    def get(self):
        return self.value
    def set(self, value):
        self.value = value


class Widget(dict):
    def config(self, **kwargs):
        self.update(kwargs)


class Root:
    def __init__(self):
        self.callbacks = []
    def after(self, delay, callback, *args):
        self.callbacks.append((delay, callback, args))


class Wire:
    def __init__(self):
        self.is_open = False
        self.input = queue.Queue()
        self.writes = []
    def open(self):
        self.is_open = True
    def close(self):
        self.is_open = False
        self.input.put(OSError('Port closed'))
    def readline(self, *_args):
        try:
            data = self.input.get(timeout=.05)
        except queue.Empty:
            return b''
        if isinstance(data, Exception):
            raise data
        return data
    def write(self, data):
        self.writes.append(data.decode('ascii').strip())
    def flush(self):
        pass


class DelayedWire(Wire):
    def __init__(self):
        super().__init__()
        self.entered = threading.Event()
        self.release = threading.Event()
    def readline(self, *_args):
        self.entered.set()
        if not self.release.wait(3):
            raise TimeoutError('Test did not release old read')
        raise OSError('Delayed failure from old port')


@unittest.skipIf(BplaGcsApp is None, 'Tk/pyserial is unavailable')
class GuiConnectionTests(unittest.TestCase):
    def setUp(self):
        self.app = app = BplaGcsApp.__new__(BplaGcsApp)
        app.ser = None
        app.alive = app._connecting = False
        app._connection_id = 0
        app._serial_stop = app._serial_thread = None
        app.tx_lock = threading.Lock()
        app.rx_queue = queue.Queue()
        app.can_receiver = UsbReceiver()
        app.var_port, app.var_baud = Value('ground'), Value('115200')
        for name in ('var_conn_status', 'var_link_state', 'var_last_ack',
                     'var_tx_rx_stat', 'var_can_received'):
            setattr(app, name, Value())
        app.lbl_conn = Widget()
        app.btn_connect = Widget(text='Подключить')
        app.root = Root()
        self.logs = []
        app.append_log = lambda *args: self.logs.append(args)

    def tearDown(self):
        self.app.disconnect_serial()

    def connect(self, wire=None):
        wire = wire or Wire()
        with patch('gcs_panel.serial.Serial', return_value=wire):
            self.app.connect_serial()
        return wire

    def test_connection_requires_ground_role_and_open_session(self):
        wire = self.connect()
        self.assertTrue(self.app._connecting)
        self.assertNotIn('ПОДКЛЮЧЕНО', self.app.var_conn_status.get())
        self.app.send_line('CAN TX t1230')
        self.assertNotIn('CAN TX t1230', wire.writes)
        self.app._parse_telemetry_line('+CAN STAT: ROLE=GROUND,OPEN=0,LINK=DOWN')
        self.assertTrue(self.app._connecting)
        self.assertEqual(wire.writes, ['CAN OPEN', 'CAN STAT'])
        self.app._parse_telemetry_line('+CAN STAT: ROLE=GROUND,OPEN=1,LINK=DOWN')
        self.assertFalse(self.app._connecting)
        self.assertIn('ПОДКЛЮЧЕНО', self.app.var_conn_status.get())
        self.app.send_line('CAN TX t1230')
        self.assertEqual(wire.writes[-1], 'CAN TX t1230')

    def test_air_role_is_rejected_and_port_is_closed(self):
        wire = self.connect()
        self.app._parse_telemetry_line('+CAN STAT: ROLE=AIR,OPEN=0,LINK=UP')
        self.assertFalse(self.app.alive)
        self.assertFalse(wire.is_open)
        self.assertTrue(any('бортовой' in message[0] for message in self.logs))

    def test_unsupported_baud_is_rejected_before_open(self):
        self.app.var_baud.set('57600')
        with patch('gcs_panel.serial.Serial') as factory, patch('gcs_panel.messagebox.showerror'):
            self.app.connect_serial()
        factory.assert_not_called()
        self.assertFalse(self.app.alive)

    def test_handshake_timeout_closes_unresponsive_port(self):
        wire = self.connect()
        self.app._check_handshake(self.app._connection_id, self.app._handshake_deadline)
        self.assertFalse(self.app.alive)
        self.assertFalse(wire.is_open)

    def test_old_reader_and_callbacks_cannot_break_new_connection(self):
        old = self.connect(DelayedWire())
        old_id = self.app._connection_id
        old_deadline = self.app._handshake_deadline
        old_thread = self.app._serial_thread
        self.assertTrue(old.entered.wait(1))
        self.app.disconnect_serial()
        new = self.connect()
        old.release.set()
        old_thread.join(1)
        self.app.rx_queue.put((old_id, OSError('Late old error')))
        self.app.rx_queue.put((old_id, '+CAN STAT: ROLE=AIR,OPEN=0'))
        self.app._initialize_modem(old_id)
        self.app._check_handshake(old_id, old_deadline)
        self.app.send_line('CAN STAT', old_id)
        self.app._process_rx_queue()
        self.assertTrue(self.app.alive)
        self.assertIs(self.app.ser, new)
        self.assertTrue(new.is_open)
        self.assertEqual(new.writes, [])

    def test_reboot_gets_a_new_handshake_timeout(self):
        self.connect()
        initial_deadline = self.app._handshake_deadline
        self.app._parse_telemetry_line('+READY: GROUND CANLINK=2')
        self.app._check_handshake(self.app._connection_id, initial_deadline)
        self.assertTrue(self.app.alive)
        self.assertTrue(self.app._connecting)

    def test_reader_assembles_fragmented_frames_before_ack(self):
        wire = self.connect()
        wire.input.put(b'+CAN RX 1 t1232')
        time.sleep(.15)
        self.assertEqual(wire.writes, [])
        self.assertIsNone(self.app.can_receiver.read())
        wire.input.put(b'00FF\r\n')
        frame = self.app.can_receiver.read(timeout=1)
        self.assertIsNotNone(frame)
        self.assertEqual(frame.data, b'\x00\xff')
        # Wait for the ACK following queue acceptance, without polling the UI.
        deadline = time.monotonic() + 1
        while 'CAN ACK 1' not in wire.writes and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertIn('CAN ACK 1', wire.writes)

    def test_current_reader_error_disconnects_on_ui_thread(self):
        wire = self.connect()
        wire.input.put(OSError('Device unplugged'))
        self.app._serial_thread.join(1)
        self.app._process_rx_queue()
        self.assertFalse(self.app.alive)
        self.assertFalse(wire.is_open)


if __name__ == '__main__':
    unittest.main()
