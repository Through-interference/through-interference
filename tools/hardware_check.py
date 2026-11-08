"""Bench-only end-to-end CAN tests. CANable acts as the flight-controller peer.

Run from the repository with python -m tools.hardware_check. No flight commands.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import queue
import struct
import threading
import time
import sys

from tools.can_client import CanFrame, CanModem, encode_frame


from tools.canable import Canable


def finish_report(report, path, closers):
    active_error = sys.exc_info()[0] is not None
    for name, close in closers:
        try:
            close()
        except Exception as exc:
            report.setdefault('cleanup_errors', []).append(f'{name}: {type(exc).__name__}: {exc}')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2))
    if report.get('cleanup_errors') and not active_error:
        raise RuntimeError('Cleanup failed; see cleanup_errors in ' + str(path))


def configure_port(port, commands):
    import serial
    device = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=1, exclusive=True)
    device.dtr = device.rts = False
    device.port = port
    device.open()
    log = []
    try:
        time.sleep(1.2)
        while device.in_waiting:
            log.append(device.readline().decode(errors='replace').strip())
        for command in commands:
            device.write((command + '\r\n').encode('ascii'))
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                line = device.readline().decode(errors='replace').strip()
                if line:
                    log.append(line)
                if line == 'OK':
                    break
                if line.startswith('ERROR:'):
                    raise RuntimeError(command + ': ' + line)
            else:
                raise TimeoutError(command + ' returned no OK')
            time.sleep(.25)
    finally:
        device.close()
    return log


def expect(read, expected, timeout=20):
    got = []
    deadline = time.monotonic() + timeout
    while len(got) < len(expected) and time.monotonic() < deadline:
        frame = read(.1)
        if frame is not None:
            got.append(frame)
    if got != expected:
        raise AssertionError({'expected': [encode_frame(f) for f in expected], 'received': [encode_frame(f) for f in got]})
    return [encode_frame(f) for f in got]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ground-port', required=True)
    parser.add_argument('--air-port', required=True)
    parser.add_argument('--bitrate', type=int, default=500000)
    parser.add_argument('--can-channel', help='Linux SocketCAN interface, default can0')
    parser.add_argument('--modulation', choices=['GFSK100', 'GFSK250', 'LORA_SF7', 'LORA_SF9'])
    parser.add_argument('--burst', type=int, default=24)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = {'tests': [], 'errors': [], 'bitrate': args.bitrate, 'modulation': args.modulation}
    canable = None
    try:
        if args.modulation:
            for role, port in [('air', args.air_port), ('ground', args.ground_port)]:
                report[role + '_setup'] = configure_port(port, ['MOD ' + args.modulation, 'SAVE', 'VER', 'CFG'])
        canable = Canable(args.bitrate, channel=args.can_channel) if args.can_channel else Canable(args.bitrate)
        with CanModem(args.ground_port, capacity=4096) as modem:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                state = modem.stats()
                if state.get('LINK') == 'UP':
                    break
                time.sleep(.3)
            else:
                raise TimeoutError('Radio peer not connected: ' + repr(state))
            report['initial_state'] = state
            vectors = [CanFrame(0x6A5, b'BAS_PING'), CanFrame(0x6A6, bytes(8)),
                       CanFrame(0x6A7, bytes.fromhex('00fffefd0a0d0080')),
                       CanFrame(0x1ABCDE01, b'\x00\xff\x80', extended=True),
                       CanFrame(0x6A8), CanFrame(0x6A9, remote=True, dlc=4),
                       CanFrame(0x1ABCDE02, extended=True, remote=True, dlc=8)]
            for index, frame in enumerate(vectors):
                started = time.monotonic()
                canable.send(frame)
                up = expect(modem.read, [frame])
                modem.send(frame)
                down = expect(canable.read, [frame])
                report['tests'].append({'name': 'frame-' + str(index), 'up': up, 'down': down,
                                        'roundtrip_seconds': round(time.monotonic() - started, 3)})
                print('PASS', encode_frame(frame), flush=True)
            # These are test stream fragments, not a claimed DroneCAN/MAVLink mapping.
            from pymavlink.dialects.v10 import ardupilotmega as v1
            from pymavlink.dialects.v20 import ardupilotmega as v2
            for name, dialect in [('MAVLink-v1', v1), ('MAVLink-v2', v2)]:
                mav = dialect.MAVLink(None, srcSystem=241, srcComponent=191)
                packet = dialect.MAVLink_heartbeat_message(18, 8, 0, 0, 3, 3).pack(mav)
                frames = [CanFrame(0x6B0, packet[i:i + 8]) for i in range(0, len(packet), 8)]
                for frame in frames:
                    canable.send(frame)
                up = expect(modem.read, frames)
                for frame in frames:
                    modem.send(frame)
                down = expect(canable.read, frames)
                report['tests'].append({'name': name, 'bytes': packet.hex(), 'up': up, 'down': down})
                print('PASS', name, len(packet), 'bytes both directions', flush=True)
            up = [CanFrame(0x6C0, i.to_bytes(4, 'little') + b'\x00\xff\xfe\xfd') for i in range(args.burst)]
            down = [CanFrame(0x6C1, i.to_bytes(4, 'little') + b'\xfd\xfe\xff\x00') for i in range(args.burst)]
            started = time.monotonic()
            for a, b in zip(up, down):
                canable.send(a)
                modem.send(b)
                time.sleep(.005)
            received_up = expect(modem.read, up, timeout=60)
            received_down = expect(canable.read, down, timeout=60)
            elapsed = time.monotonic() - started
            time.sleep(1)
            if modem.read(.3) is not None or canable.read(.3) is not None:
                raise AssertionError('Unexpected duplicate or feedback frame after stream')
            report['tests'].append({'name': 'simultaneous-burst', 'count_each_direction': args.burst,
                                    'seconds': round(elapsed, 3), 'up': received_up, 'down': received_down})
            report['final_state'] = modem.stats()
            report['ground_log'] = list(modem.logs)
            print('PASS simultaneous burst', args.burst, 'each direction in', round(elapsed, 3), 's', flush=True)
    except Exception as exc:
        report['errors'].append(f'{type(exc).__name__}: {exc}')
        raise
    finally:
        finish_report(report, args.output, [('CAN', canable.close)] if canable else [])


if __name__ == '__main__':
    main()
