"""Provision a new/reset pair over both ESP32 USB ports. Replaces both AES keys."""
from __future__ import annotations
import argparse
from contextlib import ExitStack
import os
import secrets
import sys
import time


class AtPort:
    """Exclusive AT connection. Never include command arguments in errors/logs."""
    def __init__(self, port):
        self.port = port
        self.serial = None

    def __enter__(self):
        import serial
        try:
            self.serial = serial.Serial(port=None, baudrate=115200, timeout=.1,
                                        write_timeout=1, exclusive=True)
            self.serial.dtr = self.serial.rts = False
            self.serial.port = self.port
            self.serial.open()
            time.sleep(1.5)  # Some USB-UART boards reset on open.
            self.serial.reset_input_buffer()
            return self
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.serial is not None:
            try:
                self.serial.close()
            finally:
                self.serial = None

    def __exit__(self, *args):
        self.close()

    def command(self, text, response=None, timeout=4):
        label = text.split()[0]
        try:
            self.serial.write((text + '\r\n').encode('ascii'))
        except Exception as exc:
            # A serial backend may include the bytes written in its exception.
            raise RuntimeError(f'{self.port}: {label} write failed ({type(exc).__name__})') from None
        deadline = time.monotonic() + timeout
        found = None
        while time.monotonic() < deadline:
            try:
                line = self.serial.readline().decode('ascii', errors='replace').strip()
            except Exception as exc:
                raise RuntimeError(f'{self.port}: {label} read failed ({type(exc).__name__})') from None
            # Unbound firmware emits this asynchronously until its first key.
            if line == 'ERROR: UNBOUND_USE_KEY_SAVE':
                continue
            if line.startswith('ERROR:'):
                raise RuntimeError(f'{self.port}: {label} rejected by firmware')
            if response and line.startswith(response):
                found = line[len(response):]
            if line == 'OK':
                if response and found is None:
                    raise RuntimeError(f'{self.port}: {label} response missing')
                return found
        raise TimeoutError(f'{self.port}: {label} acknowledgment missing')


def fields(text):
    try:
        return dict(part.strip().split('=', 1) for part in text.split(','))
    except (TypeError, ValueError):
        raise RuntimeError('Malformed firmware response') from None


def provision(ground_port, air_port, power):
    with ExitStack() as resources:
        ground = resources.enter_context(AtPort(ground_port))
        air = resources.enter_context(AtPort(air_port))
        for device, role in ((ground, 'GROUND'), (air, 'AIR')):
            state = fields(device.command('CAN STAT', '+CAN STAT: '))
            if state.get('ROLE') != role:
                raise RuntimeError(f'{device.port}: expected {role} firmware; no settings changed')
        # Validate both devices before changing either, then save only after every
        # setting was accepted. One key is generated for this pair, never logged.
        key = secrets.token_hex(16).upper()
        while int(key, 16) == 0:
            key = secrets.token_hex(16).upper()
        for device in (ground, air):
            for command in ('PRIMFREQ 868.0', 'BACKFREQ 869.1', 'MOD GFSK250', f'PWR {power}'):
                device.command(command)
        air.command('CANBAUD 500000')
        for device in (ground, air):
            device.command('KEY ' + key)
        for device in (ground, air):
            device.command('SAVE')
        for device in (ground, air):
            device.command('RST')
        time.sleep(2)
        expected = {'PRIM': '868.0', 'BACK': '869.1', 'MOD': 'GFSK250', 'PWR': str(power)}
        for device in (ground, air):
            device.serial.reset_input_buffer()
            cfg = fields(device.command('CFG', '+CFG: '))
            if any(cfg.get(name) != value for name, value in expected.items()):
                raise RuntimeError(f'{device.port}: saved radio profile did not reload')
        diag = fields(air.command('CAN DIAG', '+CAN DIAG: '))
        if diag.get('BAUD') != '500000':
            raise RuntimeError(f'{air.port}: saved CAN bitrate did not reload')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ground-port', required=True, help='USB serial port of ground TX ESP32')
    parser.add_argument('--air-port', required=True, help='USB serial port of air RX ESP32, not CANable')
    parser.add_argument('--new-key', action='store_true', required=True,
                        help='explicitly replace both stored keys; only for a new/reset pair')
    parser.add_argument('--power', type=int, default=20, help='TX power in dBm, -9..22 (default 20)')
    args = parser.parse_args(argv)
    if os.path.realpath(args.ground_port) == os.path.realpath(args.air_port):
        parser.error('ground and air must be distinct serial ports')
    if not -9 <= args.power <= 22:
        parser.error('--power must be between -9 and 22 dBm')
    try:
        provision(args.ground_port, args.air_port, args.power)
    except Exception as exc:
        print(f'Provisioning failed: {exc}. If settings changed, rerun for both boards.', file=sys.stderr)
        return 1
    print('Both boards saved and reloaded GFSK250, 868.0/869.1 MHz, and one new shared key; air CAN=500000. Run the CAN demo to verify the link.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
