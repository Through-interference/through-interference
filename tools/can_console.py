"""Interactive terminal for the computer/radio/CANable bench, without Tk."""
from __future__ import annotations
import argparse
from datetime import datetime
import re
import threading
import time

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
  pc 0102030405060708   отправить с компьютера на CAN борта
  bvs 0807060504030201  отправить с CANable на компьютер
  stat                  показать состояние радио и очередей
  help                  повторить подсказку
  exit                  выйти и закрыть подключения
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='/dev/cu.usbserial-10', help='ground ESP32 USB port')
    parser.add_argument('--id', default='123', help='CAN identifier in HEX, default 123')
    parser.add_argument('--extended', action='store_true')
    parser.add_argument('--bitrate', type=int, default=500000)
    parser.add_argument('--no-canable', action='store_true', help='only USB/radio, without the bench CANable peer')
    parser.add_argument('--demo', action='store_true', help='send one diagnostic frame each way, verify actual reception, then exit')
    args = parser.parse_args()
    try:
        identifier = int(args.id, 16)
        payload_frame('', identifier, args.extended)
    except ValueError as exc:
        parser.error(str(exc))
    if args.demo and args.no_canable:
        parser.error('--demo requires CANable')
    stopping = threading.Event()
    output_lock = threading.Lock()
    seen_condition = threading.Condition()
    seen = []
    errors = []
    threads = []
    canable = None

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
        with CanModem(args.port) as modem:
            if not args.no_canable:
                canable = Canable(args.bitrate)
            for read, event in [(modem.read, 'КОМПЬЮТЕР ПОЛУЧИЛ')] + (
                    [(canable.read, 'БОРТ ПОЛУЧИЛ')] if canable else []):
                thread = threading.Thread(target=monitor, args=(read, event), daemon=True)
                thread.start()
                threads.append(thread)
            output(f'USB подключён: {args.port}; CAN ID={identifier:X}; CAN={args.bitrate}')
            if canable:
                output('CANable подключён вместо БВС. Строки ПОЛУЧИЛ означают фактический приём.')
            if args.demo:
                down = payload_frame('0102030405060708', identifier, args.extended)
                up = payload_frame('0807060504030201', identifier, args.extended)
                modem.send(down)
                output(frame_text('КОМПЬЮТЕР ОТПРАВИЛ', down))
                canable.send(up)
                output(frame_text('БОРТ ОТПРАВИЛ', up))
                deadline = time.monotonic() + 20
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
            print(HELP, flush=True)
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
                    print(HELP, flush=True)
                    continue
                if line == 'stat':
                    output(str(modem.stats()))
                    continue
                command, _, text = line.partition(' ')
                try:
                    if command not in ('pc', 'bvs'):
                        raise ValueError('Неизвестная команда. Введи help')
                    if not text.strip():
                        raise ValueError('Укажи данные, например: pc 00010203')
                    frame = payload_frame(text, identifier, args.extended)
                    if command == 'pc':
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
        pass
    except Exception as exc:
        output('ОШИБКА: ' + str(exc))
        output('Если USB-порт занят, закрой панель и другие программы, использующие этот порт')
        raise SystemExit(1)
    finally:
        stopping.set()
        for thread in threads:
            thread.join(2)
        if canable:
            canable.close()


if __name__ == '__main__':
    main()
