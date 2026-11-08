import importlib.util
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class ConsoleTests(unittest.TestCase):
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
