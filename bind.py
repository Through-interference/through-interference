#!/usr/bin/env python3
import sys
import time
import serial

# === ЕДИНЫЙ ПРОФИЛЬ СОПРЯЖЕНИЯ (НАЗЕМНАЯ СТАНЦИЯ + ДРОН) ===
BIND_COMMANDS = [
    "LINK",
    "PRIMFREQ 868.0",
    "BACKFREQ 869.1",
    "MOD LORA_SF9",
    "PWR 20",
    "KEY 2B7E151628AED2A6ABF7158809CF4F3C",
    "SAVE",
    "VER",
    "RST"
]

def bind_modem(port_name: str, baudrate: int = 115200):
    print(f"[*] Подключение к {port_name} ({baudrate} бод)...")
    try:
        ser = serial.Serial(port_name, baudrate, timeout=0.5)
    except Exception as e:
        print(f"[!] Не удалось открыть порт {port_name}: {e}")
        sys.exit(1)

    # Ждем возможной перезагрузки ESP32 при открытии порта (в коде стоит delay(1000))
    time.sleep(2.0)
    ser.reset_input_buffer()

    for cmd in BIND_COMMANDS:
        print(f" -> Команда: {cmd:<40}", end="", flush=True)
        ser.write((cmd + "\r\n").encode("utf-8"))

        deadline = time.time() + 3.0
        success = False
        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="ignore").strip()
            if not line:
                continue

            if line == "OK":
                print("[ OK ]")
                success = True
                break
            elif line.startswith("ERROR"):
                print(f"[ {line} ]")
                ser.close()
                sys.exit(1)
            elif line.startswith("+VER:"):
                print(f"\n    {line} ", end="")

        if not success:
            print("[ TIMEOUT ]")
            ser.close()
            sys.exit(1)

    print("\n[*] Ожидание перезагрузки платы и проверки загрузки профиля из NVS...")
    boot_deadline = time.time() + 10.0
    boot_verified = False
    while time.time() < boot_deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="ignore").strip()
        if line:
            print(f"    [ЛОГ ПЛАТЫ] {line}")
        if (
            "Loaded and applied bind config from NVS" in line
            or "Bind config applied successfully" in line
        ):
            boot_verified = True
            break

    ser.close()
    if boot_verified:
        print(f"\n[+] УСПЕХ: Плата на {port_name} сопряжена и применила конфиг из NVS при старте!")
    else:
        print(f"\n[!] Команды записаны, но лог старта не пойман (проверьте монитор порта).")

if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
    bind_modem(port)