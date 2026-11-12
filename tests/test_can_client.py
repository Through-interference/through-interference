"""Catch binary corruption, duplicate USB delivery and false queue success."""
import importlib.util
import pathlib
import sys
import unittest
import os
import select
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('tools.can_client'), 'USB CAN client is missing')
        from tools import can_client
        self.api = can_client

    def test_can_wire_fixtures(self):
        api = self.api
        f = api.CanFrame(0x123, bytes.fromhex('00fffe fd0a0d0080'))
        self.assertEqual(api.encode_frame(f), 't123800FFFEFD0A0D0080')
        decoded = api.decode_frame('T1ABCDE01800FFFEFD0A0D0080')
        self.assertEqual(decoded.can_id, 0x1ABCDE01)
        self.assertTrue(decoded.extended)
        self.assertEqual(decoded.data, f.data)
        self.assertEqual(api.encode_frame(api.CanFrame(0x321, remote=True, dlc=8)), 'r3218')
        self.assertEqual(api.decode_frame('t0000').data, b'')

    def test_invalid_frames_are_not_truncated_or_normalized(self):
        for text in ['', 't8000', 'T200000000', 't1239', 't1231', 't1231GG', 't12310000', 'r1231AA']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.api.decode_frame(text)
        with self.assertRaises(ValueError): self.api.CanFrame(0x123, b'123456789')
        with self.assertRaises(ValueError): self.api.CanFrame(0x800, b'')

    def test_usb_ack_follows_queue_acceptance_and_retries_are_deduplicated(self):
        state = self.api.UsbReceiver(capacity=1)
        line = '+CAN RX 7 t123200FF'
        self.assertEqual(state.feed(line), 'CAN ACK 7')
        self.assertEqual(state.feed(line), 'CAN ACK 7')
        self.assertIsNone(state.feed('+CAN RX 8 t4561FD'))
        self.assertEqual(state.read().data, b'\x00\xff')
        self.assertEqual(state.feed('+CAN RX 8 t4561FD'), 'CAN ACK 8')
        self.assertEqual(state.read().can_id, 0x456)
        self.assertIsNone(state.read())

    def test_reboot_resets_usb_delivery_sequence_and_bad_lines_get_no_ack(self):
        state = self.api.UsbReceiver()
        self.assertEqual(state.feed('+CAN RX 1 t1231FF'), 'CAN ACK 1')
        self.assertIsNotNone(state.read())
        self.assertIsNone(state.feed('+READY: GROUND CANLINK=2'))
        self.assertEqual(state.feed('+CAN RX 1 t1231FF'), 'CAN ACK 1')
        self.assertIsNotNone(state.read())
        for line in ['+CAN RX 0 t1230', '+CAN RX 2 t1238', '+CAN RX 2 t1230 trailing', '[RADIO] log']:
            self.assertIsNone(state.feed(line))

    def test_duplicate_delivery_id_with_changed_bytes_is_rejected(self):
        state = self.api.UsbReceiver()
        state.feed('+CAN RX 1 t1231AA')
        self.assertIsNone(state.feed('+CAN RX 1 t1231BB'))
        self.assertEqual(state.read().data, b'\xaa')

    def test_real_serial_client_receives_once_and_reports_queue_rejection(self):
        # A PTY is the external USB peer; production pyserial/reader/ACK code runs unchanged.
        master, slave = os.openpty()
        port = os.ttyname(slave)
        stopping = threading.Event()
        def peer():
            pending = b''
            while not stopping.is_set():
                if not select.select([master], [], [], .1)[0]: continue
                try: pending += os.read(master, 4096)
                except OSError: return
                while b'\n' in pending:
                    command, pending = pending.split(b'\n', 1)
                    command = command.strip()
                    if command == b'CAN STAT':
                        os.write(master, b'+CAN STAT: ROLE=GROUND,OPEN=1,LINK=UP\r\nOK\r\n')
                    elif command.startswith(b'CAN TX '):
                        os.write(master, b'ERROR: CAN_QUEUE_FULL\r\n')
                    else: os.write(master, b'OK\r\n')
        thread = threading.Thread(target=peer, daemon=True); thread.start()
        try:
            with self.api.CanModem(port) as modem:
                os.write(master, b'+CAN RX 17 t123200FF\r\n+CAN RX 17 t123200FF\r\n')
                frame = modem.read(timeout=2)
                self.assertIsNotNone(frame)
                self.assertEqual(frame.data, b'\x00\xff')
                self.assertIsNone(modem.read(timeout=.2))
                with self.assertRaisesRegex(RuntimeError, 'CAN_QUEUE_FULL'):
                    modem.send(self.api.CanFrame(0x123, b'\x00'))
        finally:
            stopping.set(); thread.join(1); os.close(master); os.close(slave)

    def test_uncertain_send_disables_further_sends_on_the_connection(self):
        # A late response must never be usable as acceptance of the next command.
        class Wire:
            def write(self, _data): pass
        modem = self.api.CanModem('unused')
        modem._serial = Wire()
        with self.assertRaises(TimeoutError):
            modem.send(self.api.CanFrame(0x123, b'A'), timeout=.01)
        # Simulate a late marker from the first request, before the next caller.
        modem._queued += 1
        with self.assertRaisesRegex(RuntimeError, 'uncertain|reconnect'):
            modem.send(self.api.CanFrame(0x123, b'B'), timeout=.01)

    def test_fragmented_status_and_can_frames_survive_serial_timeout(self):
        import serial
        master, slave = os.openpty()
        modem = self.api.CanModem(os.ttyname(slave))
        modem._serial = serial.Serial(modem.port, 115200, timeout=.1, write_timeout=1)
        reader = threading.Thread(target=modem._reader, daemon=True)
        reader.start()
        peer_errors = []

        def peer():
            try:
                if not select.select([master], [], [], 2)[0]:
                    raise AssertionError('No status command')
                os.read(master, 2048)
                os.write(master, b'+CAN STAT: ROLE=GROUND,BROKEN\r\n')
                os.write(master, b'+CAN STAT: ROLE=GROUND,OP')
                time.sleep(.25)
                os.write(master, b'EN=1,LINK=UP\r\nOK\r\n+CAN RX 1 t1232')
                time.sleep(.25)
                os.write(master, b'00FF\r\n')
            except Exception as exc:
                peer_errors.append(exc)

        thread = threading.Thread(target=peer, daemon=True)
        thread.start()
        try:
            self.assertEqual(modem.stats(timeout=2), {'ROLE': 'GROUND', 'OPEN': 1, 'LINK': 'UP'})
            self.assertEqual(modem.read(timeout=2), self.api.CanFrame(0x123, b'\x00\xff'))
            self.assertTrue(select.select([master], [], [], 2)[0])
            self.assertIn(b'CAN ACK 1', os.read(master, 2048))
            self.assertIsNone(modem._io_error)
            self.assertTrue(reader.is_alive())
        finally:
            modem._stop.set()
            thread.join(2)
            reader.join(1)
            modem._serial.close()
            os.close(master)
            os.close(slave)
        self.assertEqual(peer_errors, [])

    def test_line_buffer_discards_oversized_lines_until_newline(self):
        from tools.serial_lines import SerialLines
        lines = SerialLines(limit=8)
        self.assertEqual(lines.feed(b'OK\r'), ['OK'])
        self.assertEqual(lines.feed(b'\n'), [])
        self.assertEqual(lines.feed(b'x' * 10000), [])
        self.assertLessEqual(len(lines.pending), 8)
        self.assertEqual(lines.feed(b'OK\nGOOD\n'), ['GOOD'])

    def test_malformed_status_does_not_look_like_a_valid_handshake(self):
        for line in ('+CAN STAT: ROLE=GROUND', '+CAN STAT: OPEN=1',
                     '+CAN STAT: ROLE=GROUND,OPEN=2', '+CAN STAT: ROLE=GROUND,OPEN=1,OPEN=0',
                     '+CAN STAT: ROLE=GROUND,OPEN=1,BROKEN'):
            with self.subTest(line=line), self.assertRaises(ValueError):
                self.api.parse_status(line)


if __name__ == '__main__': unittest.main()
