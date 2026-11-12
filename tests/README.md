# Тесты

Команды выполнять из корня проекта.

## Подготовка

Программные тесты, включая C++ ядро протокола:

```bash
sudo apt install build-essential
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
.venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v
```

Регрессионные тесты проверяют фрагментацию UART через PTY, повреждённые ответы, переподключение и подтверждение роли в GUI без окна Tk. Контроллер и менеджер радиоканалов собираются на хосте с аппаратными заглушками для проверки отказов RX, повторного запуска CAN и `FHSS 0`. Эти проверки не заменяют испытания реальных ESP32/SX1262/CANable.

Аппаратная серия на отдельном стенде:

```bash
.venv/bin/python -m tools.hardware_check \
  --ground-port /dev/ttyUSB0 --air-port /dev/ttyUSB1 --can-channel can0 \
  --burst 24 --output /tmp/can-results.json
```

Перед запуском настроить can0 через setup-can.sh. Параметр `--modulation GFSK250` при необходимости применяет и сохраняет этот профиль на обеих платах. Без него серия использует текущие настройки.

Проверка потери радио, заполнения очередей и перезапуска борта:

```bash
.venv/bin/python -m tools.recovery_check \
  --ground-port /dev/ttyUSB0 --air-port /dev/ttyUSB1 --can-channel can0 \
  --output /tmp/can-recovery.json
```

Тест требует GFSK250 на обеих платах и проверяет это до изменения профиля. Он временно меняет модуляцию борта и перезапускает его. Использовать только стенд с CANable вместо автопилота. Отчёты JSON сохраняются также при ошибке проверки; ошибки закрытия устройств записываются отдельно.


## Linux-окружение в Docker

```bash
docker build -t bas-can-linux-check -f tests/linux/Dockerfile .
docker run --rm --mount type=bind,src="$PWD",dst=/source,readonly \
  bas-can-linux-check bash /source/tests/linux/run.sh
```

Контейнер проверяет установку зависимостей, скрипты запуска, тесты Python/C++ и создание GUI с Tk/Xvfb. Аппаратные проверки запускаются отдельно на подключённых устройствах.
