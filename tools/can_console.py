"""Interactive terminal for the computer/radio/CANable bench, without Tk."""
from __future__ import annotations
import argparse
from contextlib import ExitStack
from datetime import datetime
import re
import threading
import time
import secrets
import sys

from tools.can_client import CanFrame, CanModem
from tools.canable import Canable


def payload_frame(text: str, can_id: int, extended: bool) -> CanFrame:
    text = ''.join(text.split())
    if not re.fullmatch(r'[0-9a-fA-F]*', text):
        raise ValueError('Вводи только цифры 0–9 и буквы A–F, без кавычек. Например: 00010203')
    if len(text) % 2:
        raise ValueError('Для каждого байта нужны две HEX-цифры. Например: 00010203')
    if len(text) > 16:
        raise ValueError('В одном CAN-кадре максимум 8 байт, то есть 16 HEX-цифр')
    return CanFrame(can_id, bytes.fromhex(text), extended=extended)


def frame_text(event: str, frame: CanFrame) -> str:
    digits = 8 if frame.extended else 3
    return f'{event} | ID={frame.can_id:0{digits}X} | DLC={frame.dlc} | DATA={frame.data.hex().upper()}'


HELP = '''Команды:
  send 00010203        отправить на другой компьютер (режим ground или air)
  pc 0102030405060708   отправить с компьютера на CAN борта
  bvs 0807060504030201  отправить с CANable на компьютер
  stat                  показать состояние радио и очередей
  help                  повторить подсказку
  exit                  выйти и закрыть подключения
'''


def ground_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    from serial.tools import list_ports
    ports = list(list_ports.comports())
    matches = [port.device for port in ports if (port.vid, port.pid) == (0x1A86, 0x7523)]
    if len(matches) == 1:
        return matches[0]
    available = ', '.join(f'{port.device}: {port.description}' for port in ports) or 'нет портов'
    raise RuntimeError('Укажи --port с портом наземного ESP32. Найдены: ' + available)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=['bench', 'ground', 'air'], default='bench',
                        help='bench: both devices locally; ground: USB ESP32 only; air: CANable only')
    parser.add_argument('--port', help='ground ESP32 USB port; auto-detect one CH340 if omitted')
    parser.add_argument('--id', default='123', help='CAN identifier in HEX, default 123')
    parser.add_argument('--extended', action='store_true')
    parser.add_argument('--bitrate', type=int, default=500000)
    parser.add_argument('--can-channel', help='Linux SocketCAN interface, default can0')
    parser.add_argument('--setup-can', action='store_true', help='configure SocketCAN if needed; may request sudo')
    parser.add_argument('--list-ports', action='store_true', help='list USB serial ports and exit')
    parser.add_argument('--no-canable', action='store_true', help='only USB/radio, without the bench CANable peer')
    parser.add_argument('--demo', action='store_true', help='send one diagnostic frame each way, verify actual reception, then exit')
    parser.add_argument('--demo-timeout', type=float, default=20, help='diagnostic round-trip deadline in seconds')
    args = parser.parse_args(argv)
    if args.list_ports:
        from serial.tools.list_ports import comports
        for port in sorted(comports()):
            print(f'{port.device}: {port.description} [{port.hwid}]')
        return
    if args.demo_timeout <= 0:
        parser.error('--demo-timeout must be positive')
    if args.no_canable:
        if args.role == 'air':
            parser.error('--no-canable cannot be used with --role air')
        args.role = 'ground'
    try:
        identifier = int(args.id, 16)
        payload_frame('', identifier, args.extended)
    except ValueError as exc:
        parser.error(str(exc))
    if args.demo and args.role != 'bench':
        parser.error('--demo requires --role bench with both devices connected locally')
    stopping = threading.Event()
    output_lock = threading.Lock()
    seen_condition = threading.Condition()
    seen = []
    errors = []
    threads = []
    canable = None
    modem = None
    resources = ExitStack()

    def output(text):
        with output_lock:
            print(datetime.now().strftime('%H:%M:%S.%f')[:-3], text, flush=True)

    def monitor(read, event):
        try:
            while not stopping.is_set():
                frame = read(.1)
                if frame is None:
                    continue
                output(frame_text(event, frame))
                if args.demo:
                    with seen_condition:
                        seen.append((event, frame))
                        seen_condition.notify_all()
        except Exception as exc:
            if not stopping.is_set():
                errors.append(exc)
                output('ОШИБКА ПРИЁМА: ' + str(exc))
                with seen_condition:
                    seen_condition.notify_all()

    try:
        if args.role != 'ground' and args.setup_can and sys.platform.startswith('linux'):
            from tools.socketcan import configure
            configure(args.can_channel or 'can0', args.bitrate)
        if args.role != 'air':
            args.port = ground_port(args.port)
            modem = resources.enter_context(CanModem(args.port))
        if args.role != 'ground':
            canable = Canable(args.bitrate, channel=args.can_channel) if args.can_channel else Canable(args.bitrate)
            resources.callback(canable.close)
        receivers = ([(modem.read, 'КОМПЬЮТЕР ПОЛУЧИЛ')] if modem else [])
        receivers += [(canable.read, 'БОРТ ПОЛУЧИЛ')] if canable else []
        for read, event in receivers:
            thread = threading.Thread(target=monitor, args=(read, event), daemon=True)
            thread.start()
            threads.append(thread)
        output(f'Режим={args.role}; CAN ID={identifier:X}; CAN={args.bitrate}')
        if modem:
            output(f'Наземный ESP32 подключён по USB: {args.port}')
        if canable:
            output('CANable подключён вместо БВС. Строки ПОЛУЧИЛ означают фактический приём.')
        if args.demo:
            nonce = secrets.token_bytes(7)
            down = CanFrame(identifier, b'\x01' + nonce, extended=args.extended)
            up = CanFrame(identifier, b'\x02' + nonce, extended=args.extended)
            modem.send(down)
            output(frame_text('КОМПЬЮТЕР ОТПРАВИЛ', down))
            canable.send(up)
            output(frame_text('БОРТ ОТПРАВИЛ', up))
            deadline = time.monotonic() + args.demo_timeout
            with seen_condition:
                while ('БОРТ ПОЛУЧИЛ', down) not in seen or ('КОМПЬЮТЕР ПОЛУЧИЛ', up) not in seen:
                    if errors:
                        raise RuntimeError(str(errors[0]))
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('Не получены оба тестовых сообщения. Проверь CAN STAT и подключение CANable')
                    seen_condition.wait(remaining)
            output('ТЕСТ ПРОШЁЛ: оба сообщения фактически приняты, байты совпадают')
            return
        help_text = HELP if args.role == 'bench' else ('Команды: send 00010203, stat, help, exit.\n'
                    'Строка ПОЛУЧИЛ появляется при фактическом приёме от другого компьютера.')
        print(help_text, flush=True)
        while not errors:
            try:
                line = input('can> ').strip()
            except EOFError:
                break
            if not line:
                continue
            if line in ('exit', 'quit'):
                break
            if line == 'help':
                print(help_text, flush=True)
                continue
            if line == 'stat':
                output(str(modem.stats()) if modem else f'CANable подключён; скорость={args.bitrate}. Проверяй связь по строкам ПОЛУЧИЛ')
                continue
            command, _, text = line.partition(' ')
            try:
                if command == 'send':
                    if args.role == 'bench':
                        raise ValueError('В режиме bench укажи направление: pc 00010203 или bvs 00010203')
                    command = 'pc' if args.role == 'ground' else 'bvs'
                if command not in ('pc', 'bvs'):
                    raise ValueError('Неизвестная команда. Введи help')
                if not text.strip():
                    raise ValueError('Укажи данные, например: send 00010203')
                frame = payload_frame(text, identifier, args.extended)
                if command == 'pc':
                    if not modem:
                        raise ValueError('Команда pc доступна на стороне ground')
                    modem.send(frame)
                    output(frame_text('КОМПЬЮТЕР ОТПРАВИЛ', frame))
                else:
                    if not canable:
                        raise ValueError('Команда bvs требует подключённого CANable')
                    canable.send(frame)
                    output(frame_text('БОРТ ОТПРАВИЛ', frame))
            except (ValueError, RuntimeError, TimeoutError) as exc:
                output('ОШИБКА: ' + str(exc))
        if errors:
            raise RuntimeError(str(errors[0]))
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as exc:
        output('ОШИБКА: ' + str(exc))
        output('Если USB-порт занят, закрой панель и другие программы, использующие этот порт')
        raise SystemExit(1)
    finally:
        stopping.set()
        for thread in threads:
            thread.join(2)
        resources.close()


if __name__ == '__main__':
    main()
