#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BPLA Ground Control Station (НСУ) — Расширенная панель управления БПЛА и радиомодемом ESP32 + SX1262.
Использует стандартную библиотеку tkinter и pyserial (pip install pyserial).
"""

import re
import os
import secrets
import threading
import queue
import time
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    raise SystemExit("Ошибка: не установлен pyserial. Выполните: pip install pyserial")


class BplaGcsApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("НСУ БПЛА — Панель управления дроном и радиоканалом (ESP32 SX1262)")
        
        # Делаем приложение на полный экран / развернутым по умолчанию
        # root.state('zoomed') работает на Windows/Linux, на macOS иногда нужен zoom
        try:
            self.root.state('zoomed')
        except tk.TclError:
            pass
        self.root.geometry("1280x800")
        self.root.minsize(1024, 720)

        # Состояние Serial
        self.ser: serial.Serial | None = None
        self.alive = False
        self.rx_queue: queue.Queue[str] = queue.Queue()
        self.tx_lock = threading.Lock()

        # Автоопрос телеметрии
        self.auto_poll_var = tk.BooleanVar(value=False)
        self.auto_sync_cfg_var = tk.BooleanVar(value=True) # Синхронизировать конфиг с NVS

        # Переменные телеметрии (Чтение)
        self.var_conn_status = tk.StringVar(value="ОТКЛЮЧЕНО")
        self.var_link_state = tk.StringVar(value="UNKNOWN")
        self.var_rssi = tk.StringVar(value="— dBm")
        self.var_snr = tk.StringVar(value="— dB")
        self.var_channel = tk.StringVar(value="Канал: 0 (— MHz)")
        self.var_last_ack = tk.StringVar(value="—")
        self.var_tx_rx_stat = tk.StringVar(value="TX: 0 | RX: 0")
        self.var_fw_ver = tk.StringVar(value="—")

        # Переменные текущей конфигурации на модеме (выводятся из NVS/AT)
        self.var_cur_prim = tk.StringVar(value="—")
        self.var_cur_back = tk.StringVar(value="—")
        self.var_cur_pwr = tk.StringVar(value="—")
        self.var_cur_mod = tk.StringVar(value="—")
        self.var_cur_slot = tk.StringVar(value="—")
        self.var_cur_fhss = tk.StringVar(value="—")

        # Переменные для ввода новых значений (AT)
        self.var_port = tk.StringVar(value="/dev/ttyUSB0")
        self.var_baud = tk.StringVar(value="115200")
        self.var_prim_freq = tk.StringVar(value="868.0")
        self.var_back_freq = tk.StringVar(value="869.1")
        self.var_pwr = tk.StringVar(value="20")
        self.var_mod = tk.StringVar(value="LORA_SF9")
        self.var_slot = tk.StringVar(value="0")
        self.var_fhss = tk.BooleanVar(value=True)
        self.var_key = tk.StringVar(value="2B7E151628AED2A6ABF7158809CF4F3C")
        self.var_custom_cmd = tk.StringVar()

        # Переменные полетных команд дрона
        self.var_target_alt = tk.StringVar(value="15")
        self.var_target_spd = tk.StringVar(value="5")
        self.var_step_dist = tk.StringVar(value="5")
        self.var_yaw_deg = tk.StringVar(value="45")
        self.var_wp_lat = tk.StringVar(value="59.4370")
        self.var_wp_lon = tk.StringVar(value="24.7536")
        self.var_wp_alt = tk.StringVar(value="25")
        self.var_servo_pwm = tk.IntVar(value=1500)

        self._setup_styles()
        self._build_ui()
        self.refresh_ports()

        # Периодические задачи GUI
        self.root.after(50, self._process_rx_queue)
        self.root.after(2000, self._auto_poll_telemetry)

    def _setup_styles(self):
        style = ttk.Style()
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("Header.TLabel", font=("Sans", 10, "bold"))
        style.configure("BigVal.TLabel", font=("Monospace", 12, "bold"))
        style.configure("TableVal.TLabel", font=("Monospace", 11, "bold"), foreground="#0f5132")
        style.configure("Cmd.TButton", font=("Sans", 9, "bold"), padding=5)
        style.configure("Nav.TButton", font=("Sans", 9, "bold"), foreground="#0b5394", padding=5)
        style.configure("Mode.TButton", font=("Sans", 9), padding=4)
        style.configure("Danger.TButton", font=("Sans", 9, "bold"), foreground="#b00020", padding=5)

    def _build_ui(self):
        # 1. Верхняя панель подключения
        top_frame = ttk.LabelFrame(self.root, text=" Подключение к НСУ (ESP32 UART) ", padding=6)
        top_frame.pack(fill=tk.X, padx=10, pady=(6, 4))

        ttk.Label(top_frame, text="Порт:").pack(side=tk.LEFT, padx=(0, 4))
        self.port_combo = ttk.Combobox(top_frame, textvariable=self.var_port, width=16)
        self.port_combo.pack(side=tk.LEFT, padx=4)
        ttk.Button(top_frame, text="Обновить", command=self.refresh_ports, width=10).pack(side=tk.LEFT, padx=4)
        ttk.Label(top_frame, text="Скорость:").pack(side=tk.LEFT, padx=(10, 4))
        baud_combo = ttk.Combobox(top_frame, textvariable=self.var_baud, values=["57600", "115200", "230400", "460800"], width=10, state="readonly")
        baud_combo.pack(side=tk.LEFT, padx=4)

        self.btn_connect = ttk.Button(top_frame, text="Подключить", command=self.toggle_connection, width=14)
        self.btn_connect.pack(side=tk.LEFT, padx=8)
        self.lbl_conn = tk.Label(top_frame, textvariable=self.var_conn_status, bg="#f8d7da", fg="#842029", font=("Sans", 10, "bold"), padx=10, pady=2)
        self.lbl_conn.pack(side=tk.LEFT, padx=6)
        ttk.Checkbutton(top_frame, text="Автоопрос (RSSI/SNR/STAT)", variable=self.auto_poll_var).pack(side=tk.RIGHT, padx=6)

        # 2. Центральная область
        main_pane = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main_pane.pack(fill=tk.BOTH, expand=True, padx=10, pady=4)

        left_col = ttk.Frame(main_pane, width=420)
        right_col = ttk.Frame(main_pane)
        main_pane.add(left_col, weight=1)
        main_pane.add(right_col, weight=3)

        # --- ЛЕВАЯ КОЛОНКА ---
        # 2.1 Текущая конфигурация в NVS (выводится наверх по запросу пользователя)
        cur_cfg_frame = ttk.LabelFrame(left_col, text=" Текущие параметры радиомодема ", padding=8)
        cur_cfg_frame.pack(fill=tk.X, pady=(0, 6))

        grid_cur = [
            ("Прайм частота:", self.var_cur_prim, "Резерв:", self.var_cur_back),
            ("Мощность (TX):", self.var_cur_pwr, "Модуляция:", self.var_cur_mod),
            ("TDMA слот:", self.var_cur_slot, "ППРЧ (FHSS):", self.var_cur_fhss),
        ]
        
        for r, (l1, v1, l2, v2) in enumerate(grid_cur):
            ttk.Label(cur_cfg_frame, text=l1).grid(row=r, column=0, sticky=tk.W, pady=2)
            ttk.Label(cur_cfg_frame, textvariable=v1, style="TableVal.TLabel").grid(row=r, column=1, sticky=tk.W, padx=(2, 10), pady=2)
            ttk.Label(cur_cfg_frame, text=l2).grid(row=r, column=2, sticky=tk.W, pady=2)
            ttk.Label(cur_cfg_frame, textvariable=v2, style="TableVal.TLabel").grid(row=r, column=3, sticky=tk.W, padx=(2, 0), pady=2)

        ttk.Button(cur_cfg_frame, text="Считать настройки из NVS (CFG)", command=lambda: self.send_line("CFG")).grid(row=len(grid_cur), column=0, columnspan=4, sticky=tk.EW, pady=(6, 0))

        # 2.2 Телеметрия
        telem_frame = ttk.LabelFrame(left_col, text=" Радиомониторинг (Телеметрия) ", padding=8)
        telem_frame.pack(fill=tk.X, pady=(0, 6))
        
        grid_items = [
            ("Состояние связи:", self.var_link_state),
            ("Сигнал (RSSI):", self.var_rssi),
            ("Сигнал/шум (SNR):", self.var_snr),
            ("Радиоканал:", self.var_channel),
            ("Последний ACK:", self.var_last_ack),
            ("Пакеты:", self.var_tx_rx_stat),
            ("Прошивка:", self.var_fw_ver),
        ]
        for idx, (label_text, var) in enumerate(grid_items):
            ttk.Label(telem_frame, text=label_text).grid(row=idx, column=0, sticky=tk.W, pady=2)
            ttk.Label(telem_frame, textvariable=var, style="BigVal.TLabel").grid(row=idx, column=1, sticky=tk.E, padx=(8, 0), pady=2)
        telem_frame.columnconfigure(1, weight=1)

        btn_row_telem = ttk.Frame(telem_frame)
        btn_row_telem.grid(row=len(grid_items), column=0, columnspan=2, sticky=tk.EW, pady=(6, 0))
        for col_i, (txt, cmd) in enumerate([
            ("LINK", lambda: self.send_line("LINK")),
            ("VER", lambda: self.send_line("VER")),
            ("RSSI/SNR", self.poll_telemetry_once),
            ("RST", lambda: self.send_line("RST")),
        ]):
            ttk.Button(btn_row_telem, text=txt, command=cmd).grid(row=0, column=col_i, sticky=tk.EW, padx=2)
            btn_row_telem.columnconfigure(col_i, weight=1)

        # 2.3 Изменение параметров
        cfg_frame = ttk.LabelFrame(left_col, text=" Ввод новых параметров (Отправка) ", padding=8)
        cfg_frame.pack(fill=tk.BOTH, expand=True)

        ttk.Label(cfg_frame, text="Прайм (MHz):").grid(row=0, column=0, sticky=tk.W, pady=3)
        ttk.Entry(cfg_frame, textvariable=self.var_prim_freq, width=10).grid(row=0, column=1, sticky=tk.E, pady=3)
        ttk.Button(cfg_frame, text="▶", width=3, command=lambda: self.send_line(f"PRIMFREQ {self.var_prim_freq.get().strip()}")).grid(row=0, column=2, padx=2)

        ttk.Label(cfg_frame, text="Резерв (MHz):").grid(row=1, column=0, sticky=tk.W, pady=3)
        ttk.Entry(cfg_frame, textvariable=self.var_back_freq, width=10).grid(row=1, column=1, sticky=tk.E, pady=3)
        ttk.Button(cfg_frame, text="▶", width=3, command=lambda: self.send_line(f"BACKFREQ {self.var_back_freq.get().strip()}")).grid(row=1, column=2, padx=2)

        ttk.Label(cfg_frame, text="Мощность:").grid(row=2, column=0, sticky=tk.W, pady=3)
        ttk.Spinbox(cfg_frame, from_=-9, to=22, textvariable=self.var_pwr, width=8).grid(row=2, column=1, sticky=tk.E, pady=3)
        ttk.Button(cfg_frame, text="▶", width=3, command=lambda: self.send_line(f"PWR {self.var_pwr.get().strip()}")).grid(row=2, column=2, padx=2)

        ttk.Label(cfg_frame, text="Модуляция:").grid(row=3, column=0, sticky=tk.W, pady=3)
        ttk.Combobox(cfg_frame, textvariable=self.var_mod, values=["LORA_SF9", "LORA_SF7", "GFSK250", "GFSK100"], width=9, state="readonly").grid(row=3, column=1, sticky=tk.E, pady=3)
        ttk.Button(cfg_frame, text="▶", width=3, command=lambda: self.send_line(f"MOD {self.var_mod.get().strip()}")).grid(row=3, column=2, padx=2)

        ttk.Label(cfg_frame, text="TDMA Слот:").grid(row=4, column=0, sticky=tk.W, pady=3)
        ttk.Combobox(cfg_frame, textvariable=self.var_slot, values=["0", "1", "2", "3"], width=9, state="readonly").grid(row=4, column=1, sticky=tk.E, pady=3)
        ttk.Button(cfg_frame, text="▶", width=3, command=lambda: self.send_line(f"SLOT {self.var_slot.get().strip()}")).grid(row=4, column=2, padx=2)

        ttk.Checkbutton(cfg_frame, text="ППРЧ (FHSS)", variable=self.var_fhss).grid(row=5, column=0, columnspan=2, sticky=tk.W, pady=3)
        ttk.Button(cfg_frame, text="▶", width=3, command=lambda: self.send_line(f"FHSS {'1' if self.var_fhss.get() else '0'}")).grid(row=5, column=2, padx=2)

        ttk.Label(cfg_frame, text="AES-128 (32 HEX):").grid(row=6, column=0, columnspan=3, sticky=tk.W, pady=(4, 1))
        ttk.Entry(cfg_frame, textvariable=self.var_key, font=("Monospace", 9)).grid(row=7, column=0, columnspan=3, sticky=tk.EW, pady=2)

        btn_row_key = ttk.Frame(cfg_frame)
        btn_row_key.grid(row=8, column=0, columnspan=3, sticky=tk.EW, pady=(2, 6))
        ttk.Button(btn_row_key, text="Сгенер. ключ", command=self.generate_random_key).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 2))
        ttk.Button(btn_row_key, text="Отправить ключ", command=lambda: self.send_line(f"KEY {self.var_key.get().strip().upper()}")).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(2, 0))

        ttk.Button(cfg_frame, text="СОХРАНИТЬ (SAVE)", style="Cmd.TButton", command=lambda: self.send_line("SAVE")).grid(row=9, column=0, columnspan=3, sticky=tk.EW, pady=3)
        ttk.Button(cfg_frame, text="Сброс к заводским", command=lambda: self.send_line("RESTORE")).grid(row=10, column=0, columnspan=3, sticky=tk.EW, pady=2)

        cfg_frame.columnconfigure(1, weight=1)

        # --- ПРАВАЯ КОЛОНКА ---
        drone_notebook = ttk.Notebook(right_col)
        drone_notebook.pack(fill=tk.X, pady=(0, 6))

        tab_flight = ttk.Frame(drone_notebook, padding=8)
        tab_nav = ttk.Frame(drone_notebook, padding=8)
        tab_payload = ttk.Frame(drone_notebook, padding=8)

        drone_notebook.add(tab_flight, text=" 1. Взлёт / Режимы ")
        drone_notebook.add(tab_nav, text=" 2. Навигация ")
        drone_notebook.add(tab_payload, text=" 3. Нагрузка ")

        self._build_tab_flight(tab_flight)
        self._build_tab_nav(tab_nav)
        self._build_tab_payload(tab_payload)

        manual_frame = ttk.LabelFrame(right_col, text=" Ручной ввод команды (CMD:... или AT) ", padding=6)
        manual_frame.pack(fill=tk.X, pady=(0, 6))
        entry_cmd = ttk.Entry(manual_frame, textvariable=self.var_custom_cmd, font=("Monospace", 10))
        entry_cmd.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 6))
        entry_cmd.bind("<Return>", lambda _e: self.send_custom_command())
        ttk.Button(manual_frame, text="Отправить в порт", style="Cmd.TButton", command=self.send_custom_command).pack(side=tk.LEFT)

        log_frame = ttk.LabelFrame(right_col, text=" Журнал обмена ", padding=6)
        log_frame.pack(fill=tk.BOTH, expand=True)

        self.txt_log = scrolledtext.ScrolledText(log_frame, wrap=tk.WORD, font=("Monospace", 9), bg="#1e1e1e", fg="#d4d4d4", insertbackground="white", state=tk.DISABLED)
        self.txt_log.pack(fill=tk.BOTH, expand=True)

        self.txt_log.tag_config("tx_cmd", foreground="#569cd6", font=("Monospace", 9, "bold"))
        self.txt_log.tag_config("ack_ok", foreground="#4ec9b0", font=("Monospace", 9, "bold"))
        self.txt_log.tag_config("warn", foreground="#ce9178")
        self.txt_log.tag_config("error", foreground="#f44747", font=("Monospace", 9, "bold"))
        self.txt_log.tag_config("info", foreground="#dcdcaa")

        log_ctrl = ttk.Frame(log_frame)
        log_ctrl.pack(fill=tk.X, pady=(4, 0))
        ttk.Button(log_ctrl, text="Очистить журнал", command=self.clear_log).pack(side=tk.RIGHT)

    def _build_tab_flight(self, parent: ttk.Frame):
        ttk.Label(parent, text="Управление двигателями и взлётом:", style="Header.TLabel").grid(row=0, column=0, columnspan=4, sticky=tk.W, pady=(0, 4))
        for c, (txt, cmd, st) in enumerate([("ARM", "CMD:ARM", "Cmd.TButton"), ("DISARM", "CMD:DISARM", "Danger.TButton"), ("LAND", "CMD:LAND", "Cmd.TButton"), ("EMERGENCY KILL", "CMD:ESTOP", "Danger.TButton")]):
            ttk.Button(parent, text=txt, style=st, command=lambda p=cmd: self.send_line(p)).grid(row=1, column=c, sticky=tk.EW, padx=3, pady=3)

        param_row = ttk.Frame(parent)
        param_row.grid(row=2, column=0, columnspan=4, sticky=tk.EW, pady=6)
        ttk.Label(param_row, text="Высота (м):").pack(side=tk.LEFT, padx=(2, 4))
        ttk.Spinbox(param_row, from_=1, to=500, textvariable=self.var_target_alt, width=6).pack(side=tk.LEFT, padx=2)
        ttk.Button(param_row, text="Взлёт", style="Nav.TButton", command=lambda: self.send_line(f"CMD:TAKEOFF:{self.var_target_alt.get().strip()}")).pack(side=tk.LEFT, padx=(4, 10))
        ttk.Button(param_row, text="Изменить высоту", command=lambda: self.send_line(f"CMD:ALT:{self.var_target_alt.get().strip()}")).pack(side=tk.LEFT, padx=(0, 14))
        ttk.Label(param_row, text="Скорость (м/с):").pack(side=tk.LEFT, padx=(4, 4))
        ttk.Spinbox(param_row, from_=1, to=35, textvariable=self.var_target_spd, width=5).pack(side=tk.LEFT, padx=2)
        ttk.Button(param_row, text="Задать", command=lambda: self.send_line(f"CMD:SPD:{self.var_target_spd.get().strip()}")).pack(side=tk.LEFT, padx=4)

        ttk.Label(parent, text="Полетные режимы:", style="Header.TLabel").grid(row=3, column=0, columnspan=4, sticky=tk.W, pady=(6, 4))
        modes = [("STABILIZE", "CMD:MODE:STAB"), ("ALT HOLD", "CMD:MODE:ALTHOLD"), ("LOITER", "CMD:MODE:LOITER"), ("POSHOLD", "CMD:MODE:POSHOLD"), ("AUTO", "CMD:MODE:AUTO"), ("GUIDED", "CMD:MODE:GUIDED"), ("RTL", "CMD:RTL"), ("SMART RTL", "CMD:MODE:SMARTRTL")]
        for idx, (m_title, m_cmd) in enumerate(modes):
            ttk.Button(parent, text=m_title, style="Mode.TButton", command=lambda p=m_cmd: self.send_line(p)).grid(row=4 + (idx // 4), column=idx % 4, sticky=tk.EW, padx=3, pady=2)
        for c in range(4): parent.columnconfigure(c, weight=1)

    def _build_tab_nav(self, parent: ttk.Frame):
        dpad_frame = ttk.LabelFrame(parent, text=" Относительное смещение (м) и курс (°) ", padding=6)
        dpad_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 6))

        step_bar = ttk.Frame(dpad_frame)
        step_bar.grid(row=0, column=0, columnspan=3, sticky=tk.EW, pady=(0, 6))
        ttk.Label(step_bar, text="Шаг (м):").pack(side=tk.LEFT)
        ttk.Spinbox(step_bar, from_=1, to=100, textvariable=self.var_step_dist, width=5).pack(side=tk.LEFT, padx=4)
        ttk.Label(step_bar, text="Поворот (°):").pack(side=tk.LEFT, padx=(8, 0))
        ttk.Spinbox(step_bar, from_=5, to=180, textvariable=self.var_yaw_deg, width=5).pack(side=tk.LEFT, padx=4)

        ttk.Button(dpad_frame, text="↶ Курс -", command=lambda: self.send_line(f"CMD:YAW:-{self.var_yaw_deg.get().strip()}")).grid(row=1, column=0, sticky=tk.EW, padx=2, pady=2)
        ttk.Button(dpad_frame, text="▲ ВПЕРЁД", style="Nav.TButton", command=lambda: self.send_line(f"CMD:MOVE:FWD:{self.var_step_dist.get().strip()}")).grid(row=1, column=1, sticky=tk.EW, padx=2, pady=2)
        ttk.Button(dpad_frame, text="↷ Курс +", command=lambda: self.send_line(f"CMD:YAW:+{self.var_yaw_deg.get().strip()}")).grid(row=1, column=2, sticky=tk.EW, padx=2, pady=2)
        ttk.Button(dpad_frame, text="◀ ВЛЕВО", style="Nav.TButton", command=lambda: self.send_line(f"CMD:MOVE:LEFT:{self.var_step_dist.get().strip()}")).grid(row=2, column=0, sticky=tk.EW, padx=2, pady=2)
        ttk.Button(dpad_frame, text="■ СТОП", style="Cmd.TButton", command=lambda: self.send_line("CMD:HOLD")).grid(row=2, column=1, sticky=tk.EW, padx=2, pady=2)
        ttk.Button(dpad_frame, text="ВПРАВО ▶", style="Nav.TButton", command=lambda: self.send_line(f"CMD:MOVE:RIGHT:{self.var_step_dist.get().strip()}")).grid(row=2, column=2, sticky=tk.EW, padx=2, pady=2)
        ttk.Button(dpad_frame, text="▲ Выше (+2м)", command=lambda: self.send_line("CMD:MOVE:UP:2")).grid(row=3, column=0, sticky=tk.EW, padx=2, pady=2)
        ttk.Button(dpad_frame, text="▼ НАЗАД", style="Nav.TButton", command=lambda: self.send_line(f"CMD:MOVE:BACK:{self.var_step_dist.get().strip()}")).grid(row=3, column=1, sticky=tk.EW, padx=2, pady=2)
        ttk.Button(dpad_frame, text="▼ Ниже (-2м)", command=lambda: self.send_line("CMD:MOVE:DOWN:2")).grid(row=3, column=2, sticky=tk.EW, padx=2, pady=2)
        for c in range(3): dpad_frame.columnconfigure(c, weight=1)

        wp_frame = ttk.LabelFrame(parent, text=" Полёт на GPS-точку (Waypoint) и Миссия ", padding=6)
        wp_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        ttk.Label(wp_frame, text="Широта (Lat):").grid(row=0, column=0, sticky=tk.W, pady=2)
        ttk.Entry(wp_frame, textvariable=self.var_wp_lat, width=14).grid(row=0, column=1, sticky=tk.EW, pady=2)
        ttk.Label(wp_frame, text="Долгота (Lon):").grid(row=1, column=0, sticky=tk.W, pady=2)
        ttk.Entry(wp_frame, textvariable=self.var_wp_lon, width=14).grid(row=1, column=1, sticky=tk.EW, pady=2)
        ttk.Label(wp_frame, text="Высота (Alt, м):").grid(row=2, column=0, sticky=tk.W, pady=2)
        ttk.Entry(wp_frame, textvariable=self.var_wp_alt, width=14).grid(row=2, column=1, sticky=tk.EW, pady=2)
        ttk.Button(wp_frame, text="Лететь в точку (GOTO)", style="Nav.TButton", command=self.send_goto_waypoint).grid(row=3, column=0, columnspan=2, sticky=tk.EW, pady=(6, 4))
        
        msn_row = ttk.Frame(wp_frame)
        msn_row.grid(row=4, column=0, columnspan=2, sticky=tk.EW)
        for i, (m_txt, m_cmd) in enumerate([("Старт миссии", "CMD:MSN:START"), ("Пауза", "CMD:MSN:PAUSE"), ("Задать HOME", "CMD:SETHOME")]):
            ttk.Button(msn_row, text=m_txt, command=lambda p=m_cmd: self.send_line(p)).grid(row=0, column=i, sticky=tk.EW, padx=2)
            msn_row.columnconfigure(i, weight=1)
        wp_frame.columnconfigure(1, weight=1)

    def _build_tab_payload(self, parent: ttk.Frame):
        ttk.Label(parent, text="Управление полезной нагрузкой, подвесом и светом:", style="Header.TLabel").grid(row=0, column=0, columnspan=4, sticky=tk.W, pady=(0, 6))
        payload_cmds = [("СБРОС ГРУЗА", "CMD:PAYLOAD:DROP", "Danger.TButton"), ("ЗАКРЫТЬ ЗАМОК", "CMD:PAYLOAD:LOCK", "Cmd.TButton"), ("Свет ВКЛ", "CMD:LIGHTS:ON", "Mode.TButton"), ("Свет ВЫКЛ", "CMD:LIGHTS:OFF", "Mode.TButton"), ("Камера: 0°", "CMD:GIMBAL:0", "Mode.TButton"), ("Камера: -90°", "CMD:GIMBAL:-90", "Mode.TButton"), ("Видео СТАРТ", "CMD:CAM:REC_ON", "Mode.TButton"), ("Сделать ФОТО", "CMD:CAM:SHOT", "Mode.TButton")]
        for idx, (p_txt, p_cmd, p_style) in enumerate(payload_cmds):
            ttk.Button(parent, text=p_txt, style=p_style, command=lambda p=p_cmd: self.send_line(p)).grid(row=1 + (idx // 4), column=idx % 4, sticky=tk.EW, padx=3, pady=3)
        
        servo_bar = ttk.Frame(parent)
        servo_bar.grid(row=3, column=0, columnspan=4, sticky=tk.EW, pady=(10, 2))
        ttk.Label(servo_bar, text="Серво PWM (1000..2000 мкс):").pack(side=tk.LEFT, padx=(0, 8))
        ttk.Scale(servo_bar, from_=1000, to=2000, variable=self.var_servo_pwm, orient=tk.HORIZONTAL).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=4)
        ttk.Label(servo_bar, textvariable=self.var_servo_pwm, width=6, font=("Monospace", 10, "bold")).pack(side=tk.LEFT, padx=4)
        ttk.Button(servo_bar, text="Отправить PWM", command=lambda: self.send_line(f"CMD:SERVO:{int(self.var_servo_pwm.get())}")).pack(side=tk.LEFT, padx=4)
        for c in range(4): parent.columnconfigure(c, weight=1)

    def send_goto_waypoint(self):
        lat, lon, alt = self.var_wp_lat.get().strip(), self.var_wp_lon.get().strip(), self.var_wp_alt.get().strip()
        if not lat or not lon or not alt:
            messagebox.showwarning("Координаты", "Заполните поля Lat, Lon и Alt.")
            return
        self.send_line(f"CMD:GOTO:{lat},{lon},{alt}")

    def refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        for fallback in ("/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2"):
            if os.path.exists(fallback) and fallback not in ports:
                ports.append(fallback)
        if not ports: ports = ["/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2"]
        self.port_combo["values"] = ports
        if self.var_port.get() not in ports and ports: self.var_port.set(ports[0])

    def toggle_connection(self):
        if self.alive: self.disconnect_serial()
        else: self.connect_serial()

    def connect_serial(self):
        port, baud = self.var_port.get().strip(), int(self.var_baud.get())
        try:
            self.ser = serial.Serial(port=port, baudrate=baud, timeout=0.1)
            self.alive = True
            threading.Thread(target=self._serial_reader_loop, daemon=True).start()
            self.var_conn_status.set(f"ПОДКЛЮЧЕНО ({port})")
            self.lbl_conn.config(bg="#d1e7dd", fg="#0f5132")
            self.btn_connect.config(text="Отключить")
            self.append_log(f"--- Открыт порт {port} @ {baud} бод ---", "info")
            # Сразу после подключения запрашиваем конфиг с платы
            self.send_line("CFG")
        except Exception as e:
            messagebox.showerror("Ошибка", f"Не удалось открыть {port}:\n{e}")

    def disconnect_serial(self):
        self.alive = False
        if self.ser:
            try: self.ser.close()
            except: pass
            self.ser = None
        self.var_conn_status.set("ОТКЛЮЧЕНО")
        self.lbl_conn.config(bg="#f8d7da", fg="#842029")
        self.btn_connect.config(text="Подключить")
        self.append_log("--- Порт закрыт ---", "warn")

    def _serial_reader_loop(self):
        while self.alive and self.ser and self.ser.is_open:
            try:
                raw = self.ser.readline()
                if raw:
                    line = raw.decode("utf-8", errors="replace").strip()
                    if line: self.rx_queue.put(line)
            except Exception as e:
                self.rx_queue.put(f"[ERROR] Потеря связи: {e}")
                self.alive = False
                break

    def send_line(self, cmd: str):
        if not self.alive or not self.ser or not self.ser.is_open: return
        clean = cmd.strip()
        if not clean: return
        with self.tx_lock:
            try:
                self.ser.write((clean + "\r\n").encode("utf-8"))
                self.ser.flush()
                self.append_log(f">> {clean}", "tx_cmd")
            except Exception as e:
                self.append_log(f"[ERROR] Ошибка отправки: {e}", "error")

    def send_custom_command(self):
        cmd = self.var_custom_cmd.get().strip()
        if cmd:
            self.send_line(cmd)
            self.var_custom_cmd.set("")

    def _process_rx_queue(self):
        while not self.rx_queue.empty():
            line = self.rx_queue.get_nowait()
            self._parse_telemetry_line(line)
            tag = None
            if "ACK matched" in line or "Connection restored" in line or "OK" == line: tag = "ack_ok"
            elif "timeout" in line.lower() or "degraded" in line.lower() or "searching" in line.lower(): tag = "warn"
            elif "error" in line.lower() or "fail" in line.lower() or "blocked" in line.lower(): tag = "error"
            elif line.startswith("+"): tag = "info"
            self.append_log(line, tag)
        if not self.alive and self.btn_connect["text"] == "Отключить":
            self.disconnect_serial()
        self.root.after(50, self._process_rx_queue)

    def _parse_telemetry_line(self, line: str):
        # Парсинг стандартной телеметрии
        m_rssi = re.search(r"RSSI:\s*([-\d.]+)\s*dBm", line)
        if m_rssi: self.var_rssi.set(f"{m_rssi.group(1)} dBm")

        m_snr = re.search(r"SNR:\s*([-\d.]+)\s*dB", line)
        if m_snr: self.var_snr.set(f"{m_snr.group(1)} dB")

        m_stat = re.search(r"\+STAT:\s*TX=(\d+),\s*RX=(\d+)", line)
        if m_stat: self.var_tx_rx_stat.set(f"TX: {m_stat.group(1)} | RX: {m_stat.group(2)}")

        if line.startswith("+VER:"):
            self.var_fw_ver.set(line.replace("+VER:", "").strip())

        m_state = re.search(r"Connection state:\s*(\w+)", line)
        if m_state: self.var_link_state.set(m_state.group(1))

        m_chan = re.search(r"Channel:\s*(\d+)\s*\|\s*Frequency:\s*([\d.]+)\s*MHz", line)
        if m_chan: self.var_channel.set(f"Канал: {m_chan.group(1)} ({m_chan.group(2)} MHz)")

        m_ack = re.search(r"ACK matched messageId=(\d+)", line)
        if m_ack:
            self.var_last_ack.set(f"ID #{m_ack.group(1)}")
            self.var_link_state.set("CONNECTED")
            
        # Парсинг текущего конфига (ответ на команду CFG)
        # Формат от ESP32: +CFG: PRIM=868.0, BACK=869.1, PWR=20, MOD=LORA_SF9, SLOT=0, FHSS=1
        if line.startswith("+CFG:"):
            m_cfg = re.search(r"PRIM=([\d.]+),\s*BACK=([\d.]+),\s*PWR=([-\d]+),\s*MOD=(\w+),\s*SLOT=(\d),\s*FHSS=(\d)", line)
            if m_cfg:
                self.var_cur_prim.set(m_cfg.group(1))
                self.var_cur_back.set(m_cfg.group(2))
                self.var_cur_pwr.set(m_cfg.group(3))
                self.var_cur_mod.set(m_cfg.group(4))
                self.var_cur_slot.set(m_cfg.group(5))
                self.var_cur_fhss.set("ВКЛ" if m_cfg.group(6) == "1" else "ВЫКЛ")

                # Синхронизация полей ввода с текущим конфигом, если стоит галочка
                if self.auto_sync_cfg_var.get():
                    self.var_prim_freq.set(m_cfg.group(1))
                    self.var_back_freq.set(m_cfg.group(2))
                    self.var_pwr.set(m_cfg.group(3))
                    self.var_mod.set(m_cfg.group(4))
                    self.var_slot.set(m_cfg.group(5))
                    self.var_fhss.set(True if m_cfg.group(6) == "1" else False)

    def generate_random_key(self):
        self.var_key.set(secrets.token_hex(16).upper())

    def poll_telemetry_once(self):
        if not self.alive: return
        def _poll():
            for cmd in ("RSSI", "SNR", "STAT"):
                self.send_line(cmd)
                time.sleep(0.06)
        threading.Thread(target=_poll, daemon=True).start()

    def _auto_poll_telemetry(self):
        if self.alive and self.auto_poll_var.get():
            self.poll_telemetry_once()
        self.root.after(2000, self._auto_poll_telemetry)

    def append_log(self, text: str, tag: str | None = None):
        self.txt_log.config(state=tk.NORMAL)
        self.txt_log.insert(tk.END, text + "\n", tag) if tag else self.txt_log.insert(tk.END, text + "\n")
        self.txt_log.see(tk.END)
        self.txt_log.config(state=tk.DISABLED)

    def clear_log(self):
        self.txt_log.config(state=tk.NORMAL)
        self.txt_log.delete("1.0", tk.END)
        self.txt_log.config(state=tk.DISABLED)


if __name__ == "__main__":
    root = tk.Tk()
    app = BplaGcsApp(root)
    root.mainloop()