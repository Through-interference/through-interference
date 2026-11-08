"""Linux SocketCAN transport and explicit interface setup for the CAN bench."""
from __future__ import annotations
import json
import os
import re
import subprocess
import time

from tools.can_client import CanFrame


def read_link(channel):
    if not re.fullmatch(r'[A-Za-z0-9_.:-]{1,15}', channel):
        raise ValueError('Неверное имя CAN-интерфейса')
    try:
        result = subprocess.run(['ip', '-details', '-json', 'link', 'show', 'dev', channel],
                                capture_output=True, text=True, check=True)
        return json.loads(result.stdout)[0]
    except FileNotFoundError as exc:
        raise RuntimeError('Установи iproute2. Ubuntu/Debian: sudo apt install iproute2 can-utils') from exc
    except (subprocess.CalledProcessError, ValueError, IndexError) as exc:
        raise RuntimeError(f'CAN-интерфейс {channel} не найден. Подключи CANable с gs_usb; проверь ip -br link') from exc


def active_modes(link):
    modes = link.get('linkinfo', {}).get('info_data', {}).get('ctrlmode', [])
    if isinstance(modes, dict):
        modes = modes.get('flags', [])
    return {str(mode).upper() for mode in modes} if isinstance(modes, list) else set()


def validate_link(link, channel, bitrate):
    info = link.get('linkinfo', {})
    kind = info.get('info_kind')
    if kind not in ('can', 'vcan'):
        raise RuntimeError(f'{channel} не является CAN-интерфейсом')
    if 'UP' not in link.get('flags', []):
        raise RuntimeError(f'{channel} выключен. Запусти ./setup-can.sh {channel} {bitrate}')
    data = info.get('info_data', {})
    if kind == 'can' and data.get('bittiming', {}).get('bitrate') != bitrate:
        raise RuntimeError(f'Скорость {channel} отличается от {bitrate}. Запусти ./setup-can.sh {channel} {bitrate}')
    if data.get('state') in ('BUS-OFF', 'STOPPED'):
        raise RuntimeError(f'{channel}: {data["state"]}. Проверь CAN-H/CAN-L, питание и терминацию')
    if active_modes(link) & {'LISTEN-ONLY', 'LOOPBACK', 'FD', 'FD-NON-ISO'}:
        raise RuntimeError(f'{channel}: нужен normal Classical CAN без listen-only/loopback/FD')


def configure(channel='can0', bitrate=500000):
    if bitrate not in (125000, 250000, 500000, 1000000):
        raise ValueError('Поддерживаются CAN 125000, 250000, 500000, 1000000')
    link = read_link(channel)
    try:
        validate_link(link, channel, bitrate)
        return
    except RuntimeError:
        pass
    kind = link.get('linkinfo', {}).get('info_kind')
    if kind not in ('can', 'vcan'):
        raise RuntimeError(f'{channel} не является CAN-интерфейсом')
    prefix = [] if os.geteuid() == 0 else ['sudo']
    print(f'Настройка {channel}, {bitrate} бит/с. Для изменения интерфейса могут потребоваться права sudo.', flush=True)
    commands = [['ip', 'link', 'set', 'dev', channel, 'down']]
    if kind == 'can':
        settings = ['ip', 'link', 'set', 'dev', channel, 'type', 'can',
                    'bitrate', str(bitrate), 'restart-ms', '100']
        for mode, option in [('LISTEN-ONLY', 'listen-only'), ('LOOPBACK', 'loopback'),
                             ('FD-NON-ISO', 'fd-non-iso'), ('FD', 'fd')]:
            if mode in active_modes(link):
                settings += [option, 'off']
        commands.append(settings)
    commands.append(['ip', 'link', 'set', 'dev', channel, 'up'])
    try:
        for command in commands:
            subprocess.run(prefix + command, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        raise RuntimeError('Не удалось настроить CAN-интерфейс. Проверь права и вывод ip выше') from exc
    validate_link(read_link(channel), channel, bitrate)


class SocketCanable:
    def __init__(self, bitrate=500000, channel='can0', *, bus_factory=None, link_reader=None):
        import can
        validate_link((link_reader or read_link)(channel), channel, bitrate)
        self.bus = (bus_factory or can.Bus)(interface='socketcan', channel=channel,
                                           receive_own_messages=False, ignore_config=True, fd=False)
        self.closed = False

    def send(self, frame):
        import can
        self.bus.send(can.Message(arbitration_id=frame.can_id, data=frame.data,
                                  is_extended_id=frame.extended, is_remote_frame=frame.remote,
                                  dlc=frame.dlc, check=True), timeout=1)

    def read(self, timeout=.1):
        deadline = time.monotonic() + max(0, timeout)
        while True:
            message = self.bus.recv(max(0, deadline - time.monotonic()))
            if message is None:
                return None
            if message.is_rx and not message.is_error_frame:
                if message.is_fd:
                    raise RuntimeError('CAN FD не поддерживается: нужен Classical CAN')
                return CanFrame(message.arbitration_id, b'' if message.is_remote_frame else bytes(message.data),
                                message.is_extended_id, message.is_remote_frame, message.dlc)
            if time.monotonic() >= deadline:
                return None

    def close(self):
        if not self.closed:
            self.closed = True
            self.bus.shutdown()


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--channel', default='can0')
    parser.add_argument('--bitrate', type=int, default=500000)
    args = parser.parse_args()
    configure(args.channel, args.bitrate)
    print(json.dumps(read_link(args.channel), indent=2))
