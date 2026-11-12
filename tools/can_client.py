"""USB CAN transport. Frames use SLCAN notation; commands are CANLINK v2."""
from __future__ import annotations
from collections import deque
from dataclasses import dataclass
import queue
import re
import threading
import time
from tools.serial_lines import SerialLines


def parse_status(line: str) -> dict:
    if not line.startswith('+CAN STAT: '):
        raise ValueError('Not a CAN status response')
    state = {}
    for field in line[len('+CAN STAT: '):].split(','):
        key, separator, value = field.partition('=')
        if not separator or not re.fullmatch(r'[A-Z_]+', key) or not value or key in state:
            raise ValueError('Malformed CAN status response')
        state[key] = int(value) if value.isdecimal() else value
    if state.get('ROLE') not in ('GROUND', 'AIR') or state.get('OPEN') not in (0, 1):
        raise ValueError('CAN status is missing its role or USB state')
    return state


@dataclass(frozen=True)
class CanFrame:
    can_id: int
    data: bytes = b''
    extended: bool = False
    remote: bool = False
    dlc: int | None = None

    def __post_init__(self):
        object.__setattr__(self, 'data', bytes(self.data))
        dlc = len(self.data) if self.dlc is None else self.dlc
        if not 0 <= self.can_id <= (0x1FFFFFFF if self.extended else 0x7FF):
            raise ValueError('CAN ID outside its frame format')
        if not 0 <= dlc <= 8 or (self.remote and self.data) or (not self.remote and dlc != len(self.data)):
            raise ValueError('Classical CAN requires DLC 0..8 and matching data')
        object.__setattr__(self, 'dlc', dlc)


def encode_frame(frame: CanFrame) -> str:
    prefix = ('R' if frame.extended else 'r') if frame.remote else ('T' if frame.extended else 't')
    digits = 8 if frame.extended else 3
    return f'{prefix}{frame.can_id:0{digits}X}{frame.dlc}' + frame.data.hex().upper()


def decode_frame(text: str) -> CanFrame:
    if not isinstance(text, str) or len(text) < 5 or text[0] not in 'tTrR':
        raise ValueError('Invalid CAN frame')
    extended = text[0] in 'TR'
    remote = text[0] in 'rR'
    digits = 8 if extended else 3
    if len(text) < digits + 2 or not re.fullmatch(r'[0-9A-Fa-f]+', text[1:digits + 1]):
        raise ValueError('Invalid CAN ID')
    if text[digits + 1] not in '012345678':
        raise ValueError('Invalid DLC')
    dlc = int(text[digits + 1])
    body = text[digits + 2:]
    if len(body) != (0 if remote else 2 * dlc) or (body and not re.fullmatch(r'[0-9A-Fa-f]+', body)):
        raise ValueError('CAN payload length or encoding is invalid')
    return CanFrame(int(text[1:digits + 1], 16), bytes.fromhex(body), extended, remote, dlc)


class UsbReceiver:
    """ACK only frames retained for the application; suppress USB retransmissions."""
    def __init__(self, capacity: int = 1024):
        self._queue = queue.Queue(maxsize=capacity)
        self._last = None

    def feed(self, line: str) -> str | None:
        if line.startswith('+READY: GROUND'):
            self._last = None
            return None
        match = re.fullmatch(r'\+CAN RX ([0-9]+) ([tTrR][0-9A-Fa-f]+)', line)
        if not match:
            return None
        delivery = int(match[1])
        if not 1 <= delivery <= 0xFFFFFFFF:
            return None
        try:
            frame = decode_frame(match[2])
        except ValueError:
            return None
        if self._last and self._last[0] == delivery:
            return f'CAN ACK {delivery}' if self._last[1] == frame else None
        try:
            self._queue.put_nowait(frame)
        except queue.Full:
            return None
        self._last = (delivery, frame)
        return f'CAN ACK {delivery}'

    def read(self, timeout: float = 0) -> CanFrame | None:
        try:
            return self._queue.get(timeout=max(0, timeout))
        except queue.Empty:
            return None


class CanModem:
    """One serial owner; reader handles telemetry ACKs while callers send frames."""
    def __init__(self, port: str, baudrate: int = 115200, capacity: int = 1024):
        self.port = port
        self.baudrate = baudrate
        self.receiver = UsbReceiver(capacity)
        self.logs = deque(maxlen=100)
        self._stop = threading.Event()
        self._write_lock = threading.Lock()
        self._send_lock = threading.Lock()
        self._condition = threading.Condition()
        self._queued = self._error_number = self._status_number = 0
        self._status = {}
        self._last_error = None
        self._io_error = None
        self._send_uncertain = False
        self._serial = None
        self._threads = []

    def _write(self, line):
        with self._write_lock:
            self._serial.write((line + '\r\n').encode('ascii'))

    def open(self):
        import serial
        self._serial = serial.Serial(port=None, baudrate=self.baudrate, timeout=.1, write_timeout=1, exclusive=True)
        self._serial.dtr = False
        self._serial.rts = False
        self._serial.port = self.port
        self._serial.open()
        reader = threading.Thread(target=self._reader, name='can-usb-reader', daemon=True)
        reader.start()
        self._threads.append(reader)
        # CH340 boards can reset when opening their port. Wait before service commands.
        time.sleep(1.3)
        self._write('CAN OPEN')
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            state = self.stats(timeout=1)
            if state.get('ROLE') != 'GROUND':
                raise RuntimeError('Select the ground TX ESP32 serial port')
            if state.get('OPEN') == 1:
                keeper = threading.Thread(target=self._keepalive, name='can-usb-lease', daemon=True)
                keeper.start()
                self._threads.append(keeper)
                return self
            self._write('CAN OPEN')
        raise TimeoutError('Ground modem did not open the CAN USB session')

    def _reader(self):
        lines = SerialLines()
        try:
            while not self._stop.is_set():
                raw = self._serial.readline(2048)
                for line in lines.feed(raw):
                    self.logs.append(line)
                    ack = self.receiver.feed(line)
                    if ack:
                        self._write(ack)
                    if line.startswith('+READY: GROUND'):
                        self._write('CAN OPEN')
                    with self._condition:
                        if line == '+CAN QUEUED':
                            self._queued += 1
                        elif line.startswith('ERROR:'):
                            self._last_error = line
                            self._error_number += 1
                        elif line.startswith('+CAN STAT: '):
                            try:
                                self._status = parse_status(line)
                            except ValueError:
                                continue
                            self._status_number += 1
                        self._condition.notify_all()
        except Exception as exc:
            if not self._stop.is_set():
                with self._condition:
                    self._io_error = exc
                    self._condition.notify_all()

    def _keepalive(self):
        while not self._stop.wait(1.5):
            try:
                self._write('CAN OPEN')
            except Exception as exc:
                self._io_error = exc
                return

    def send(self, frame: CanFrame, timeout: float = 3):
        """Wait for local queue acceptance, not an autopilot execution result."""
        with self._send_lock, self._condition:
            if self._send_uncertain:
                raise RuntimeError('Previous USB send is uncertain; reconnect before sending more frames')
            previous = self._queued
            errors = self._error_number
            self._write('CAN TX ' + encode_frame(frame))
            deadline = time.monotonic() + timeout
            while self._queued == previous:
                if self._io_error:
                    raise OSError(str(self._io_error))
                if self._error_number != errors:
                    raise RuntimeError(self._last_error)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self._send_uncertain = True
                    raise TimeoutError('USB queue acknowledgment missing; delivery is uncertain, frame not resent')
                self._condition.wait(remaining)

    def read(self, timeout: float = 0) -> CanFrame | None:
        frame = self.receiver.read(timeout)
        if frame is None and self._io_error:
            raise OSError(str(self._io_error))
        return frame

    def stats(self, timeout: float = 2) -> dict:
        with self._condition:
            previous = self._status_number
            self._write('CAN STAT')
            deadline = time.monotonic() + timeout
            while self._status_number == previous:
                if self._io_error:
                    raise OSError(str(self._io_error))
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('CAN STAT response missing')
                self._condition.wait(remaining)
            return dict(self._status)

    def close(self):
        if self._serial:
            try:
                self._write('CAN CLOSE')
            except Exception:
                pass
        self._stop.set()
        for thread in self._threads:
            thread.join(timeout=2)
        if self._serial:
            self._serial.close()
            self._serial = None

    def __enter__(self):
        try:
            return self.open()
        except BaseException:
            self.close()
            raise

    def __exit__(self, *args):
        self.close()


def main():
    import argparse
    import json
    parser = argparse.ArgumentParser(description='Send and receive raw CAN frames through the ground ESP32')
    parser.add_argument('--port', required=True)
    parser.add_argument('--send', help='SLCAN frame, e.g. t123200FF or T1ABCDE0180011223344556677')
    parser.add_argument('--seconds', type=float, default=10)
    args = parser.parse_args()
    with CanModem(args.port) as modem:
        if args.send:
            modem.send(decode_frame(args.send))
            print('Frame accepted by the ground radio queue', flush=True)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            frame = modem.read(timeout=.2)
            if frame:
                print(encode_frame(frame), flush=True)
        print(json.dumps(modem.stats(), ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
