# Проверка обмена с Linux

Linux подключается по USB к наземному ESP32. Бортовой ESP32 с CAN-модулем и CANable остаются на Mac. На Mac запустить `start-air.command`.

```text
Linux ↔ USB ↔ наземный ESP32 ↔ радио ↔ бортовой ESP32 ↔ CAN ↔ CANable ↔ Mac
```

## Запуск

Из папки проекта:

```bash
git switch main
git pull --ff-only
./start-ground.sh
```

Скрипт создаёт локальное Python-окружение `.console-venv`, при необходимости устанавливает pyserial и открывает консоль земли. При первом запуске для установки зависимости нужен интернет. Нужен Python 3.10 или новее с модулем venv. Для Ubuntu/Debian, если его нет:

```bash
sudo apt update
sudo apt install python3 python3-venv
```

Скрипт ищет единственный USB-UART CH340 с VID:PID 1a86:7523. Если подключено несколько плат, указать порт своей наземной платы:

```bash
./start-ground.sh --port /dev/ttyUSB0
```

Для списка портов после подготовки окружения:

```bash
.console-venv/bin/python -m serial.tools.list_ports -v
```

## Обмен

В консоли Linux:

```text
send 00010203
```

На Mac ожидается `БОРТ ПОЛУЧИЛ | ID=123 | DLC=4 | DATA=00010203`.

В консоли Mac:

```text
send 04050607
```

На Linux ожидается `КОМПЬЮТЕР ПОЛУЧИЛ | ID=123 | DLC=4 | DATA=04050607`.

`stat` показывает состояние радио и очередей. `exit` закрывает консоль. Подтверждение прохождения всего пути появляется в строке `ПОЛУЧИЛ` на другой стороне.

## Если нет прав на порт

При `Permission denied` проверить группу устройства:

```bash
ls -l /dev/ttyUSB0
```

На системах с группой `dialout`:

```bash
sudo usermod -aG dialout "$USER"
```

Затем перезагрузить компьютер или завершить сеанс и войти заново. Если у устройства другая группа, например `uucp`, использовать её вместо `dialout`. Это соответствует [инструкции по доступу к serial-портам Linux](https://support.arduino.cc/hc/en-us/articles/360016495679-Fix-port-access-on-Linux). Программу после выдачи прав запускать от обычного пользователя.

Этот скрипт работает с USB наземного ESP32. Он не настраивает CANable на Linux; в этой схеме CANable подключён к Mac.
