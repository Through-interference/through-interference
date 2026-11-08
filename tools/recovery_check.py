"""Bench-only failure tests, using CANable as the remote CAN peer."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import time

from tools.can_client import CanFrame, CanModem
from tools.hardware_check import Canable, expect


class AirService:
    def __init__(self, port):
        import serial
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=.05, write_timeout=1, exclusive=True)
        self.serial.dtr = self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        self.logs = []
        time.sleep(1.3)
        while self.serial.in_waiting:
            self.logs.append(self.serial.readline().decode(errors='replace').strip())

    def command(self, text, timeout=3):
        self.serial.write((text + '\r\n').encode('ascii'))
        deadline = time.monotonic() + timeout
        result = []
        while time.monotonic() < deadline:
            line = self.serial.readline().decode(errors='replace').strip()
            if line:
                result.append(line)
                self.logs.append(line)
            if line == 'OK':
                return result
            if line.startswith('ERROR:'):
                raise RuntimeError(text + ': ' + line)
        raise TimeoutError(text + ' not acknowledged')

    def close(self):
        self.serial.close()


def wait_link(modem, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = modem.stats()
        if state.get('LINK') == 'UP':
            return state
        time.sleep(.2)
    raise TimeoutError('Radio link did not recover: ' + repr(state))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ground-port', required=True)
    parser.add_argument('--air-port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = {'tests': [], 'errors': []}
    air = canable = None
    try:
        air = AirService(args.air_port)
        canable = Canable()
        with CanModem(args.ground_port, capacity=1) as modem:
            wait_link(modem)
            # Freeze physical radio reception without changing the saved profile.
            air.command('MOD LORA_SF7')
            time.sleep(.5)
            up = [CanFrame(0x6D0, bytes([i, 0, 0xFF])) for i in range(3)]
            down = [CanFrame(0x6D1, bytes([i, 0xFF, 0])) for i in range(3)]
            for a, b in zip(up, down):
                canable.send(a)
                modem.send(b)
            time.sleep(1.3)
            disconnected = modem.stats()
            if disconnected['TX_PENDING'] != 3:
                raise AssertionError('Unacknowledged frames were discarded during radio outage')
            air.command('MOD GFSK250')
            expect(modem.read, up, timeout=15)
            expect(canable.read, down, timeout=15)
            report['tests'].append({'name': 'radio-outage-retains-both-queues', 'during_outage': disconnected,
                                    'after_recovery': wait_link(modem)})
            print('PASS radio outage retained both directions', flush=True)
            # The application queue holds just one frame. USB ACKs must stop before overflow.
            paused = [CanFrame(0x6D2, bytes([i, 0x80])) for i in range(3)]
            for f in paused:
                canable.send(f)
            time.sleep(.9)
            state = modem.stats()
            if state['USB_PENDING'] < 1:
                raise AssertionError('USB discarded frames while the application queue was full')
            expect(modem.read, paused, timeout=10)
            report['tests'].append({'name': 'slow-usb-application-backpressure', 'while_paused': state})
            print('PASS USB backpressure retained all frames', flush=True)
            # Fill the 64-frame ground radio queue while the peer cannot hear it.
            time.sleep(.2)
            air.command('MOD LORA_SF7')
            time.sleep(.4)
            full = [CanFrame(0x6E0, i.to_bytes(4, 'little') + b'\x00\xff') for i in range(64)]
            for f in full:
                modem.send(f)
            rejected = False
            try:
                modem.send(CanFrame(0x6E0, b'OVERFLOW'))
            except RuntimeError as exc:
                rejected = 'CAN_QUEUE_FULL' in str(exc)
            if not rejected:
                raise AssertionError('65th queued frame was not explicitly rejected')
            air.command('MOD GFSK250')
            expect(canable.read, full, timeout=40)
            report['tests'].append({'name': 'bounded-queue-64-plus-explicit-rejection', 'state': wait_link(modem)})
            print('PASS queue full is explicit; retained 64 frames delivered', flush=True)
            # Restart immediately after physical CAN reception, before its radio ACK if possible.
            reset_results = []
            for trial in range(3):
                before = modem.stats()['UNCERTAIN']
                frame = CanFrame(0x6EF, b'RESET' + bytes([trial]))
                modem.send(frame)
                expect(canable.read, [frame], timeout=10)
                air.command('RST')
                time.sleep(1.5)
                after = wait_link(modem, timeout=20)
                extra = canable.read(.8)
                if extra is not None:
                    raise AssertionError('A delivered CAN frame was duplicated after receiver restart')
                reset_results.append({'trial': trial, 'uncertain_before': before, 'state': after})
                if after['UNCERTAIN'] > before:
                    break
            report['tests'].append({'name': 'receiver-restart-no-command-replay', 'trials': reset_results,
                                    'lost_ack_window_observed': any(r['state']['UNCERTAIN'] > r['uncertain_before'] for r in reset_results)})
            print('PASS receiver restart without command replay', flush=True)
            # Traffic still works in the new destination session.
            frame = CanFrame(0x6F0, b'AFTER\x00\xff')
            canable.send(frame)
            expect(modem.read, [frame])
            modem.send(frame)
            expect(canable.read, [frame])
            time.sleep(.3)
            report['final_state'] = modem.stats()
            report['air_state'] = air.command('CAN STAT')
            report['ground_log'] = list(modem.logs)
            report['air_log'] = air.logs[-100:]
            print('PASS traffic after recovery both directions', flush=True)
    except Exception as exc:
        report['errors'].append(f'{type(exc).__name__}: {exc}')
        raise
    finally:
        if canable:
            canable.close()
        if air:
            air.close()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
