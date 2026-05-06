import queue
import json
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
import urllib.request
import webbrowser
from pathlib import Path
from tkinter import messagebox, ttk

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


BAUDRATE = 115200
STATUS_INTERVAL_MS = 250
APP_VERSION = "0.2.0"

APP_UPDATE_REPO = "JuanEGamerYT/volante-config"
APP_UPDATE_DIR = ".updates"
APP_UPDATE_MARKER = ".pending_update.json"

RGB_MODE_LABELS = {
    "OFF": "Desactivado",
    "FIXED": "Fijo",
    "CYCLE": "Ciclo",
    "BREATH": "Respiracion",
    "WAVE": "Onda de colores",
    "PULSE": "Punto de pulsar",
}
RGB_MODE_COMMANDS = {label: command for command, label in RGB_MODE_LABELS.items()}


def parse_kv_line(line):
    data = {}
    for token in line.split()[1:]:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        data[key.strip()] = value.strip()
    return data


def clamp(value, low, high):
    return max(low, min(high, value))


def version_tuple(version):
    clean = version.strip().lstrip("vV")
    parts = []
    for piece in clean.split("."):
        number = ""
        for char in piece:
            if not char.isdigit():
                break
            number += char
        parts.append(int(number or 0))
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:3])


class SerialClient:
    def __init__(self, port, on_line):
        self.port = port
        self.on_line = on_line
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.ser = serial.Serial(port, BAUDRATE, timeout=0.05, write_timeout=0.5)
        self.thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.thread.start()

    def _reader_loop(self):
        while not self.stop_event.is_set():
            try:
                raw = self.ser.readline()
            except serial.SerialException as exc:
                self.on_line(f"APP ERROR serial: {exc}")
                break

            if not raw:
                continue

            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                self.on_line(line)

    def send(self, command):
        if not command:
            return
        with self.lock:
            self.ser.write((command.strip() + "\n").encode("ascii", errors="ignore"))

    def close(self):
        self.stop_event.set()
        try:
            self.send("QUIET 0")
            time.sleep(0.05)
        except Exception:
            pass
        try:
            self.ser.close()
        except Exception:
            pass


class WheelConfigApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Volante Config")
        self.geometry("1040x720")
        self.minsize(940, 640)

        self.serial_client = None
        self.line_queue = queue.Queue()
        self.config = {}
        self.status = {}
        self.port_values = []
        self.ota_upload_running = False
        self.update_check_running = False

        self._build_style()
        self._build_variables()
        self._build_ui()
        self.refresh_ports()
        self.after(100, self._drain_lines)
        self.after(STATUS_INTERVAL_MS, self._poll_status)
        self.after(1500, lambda: self.check_for_app_updates(silent=True))

    def _build_style(self):
        self.configure(bg="#11151c")
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure(".", background="#11151c", foreground="#edf2f7", fieldbackground="#1c2430")
        style.configure("TFrame", background="#11151c")
        style.configure("Card.TFrame", background="#18202b", relief="flat")
        style.configure("TLabel", background="#11151c", foreground="#edf2f7")
        style.configure("Card.TLabel", background="#18202b", foreground="#edf2f7")
        style.configure("Muted.TLabel", background="#18202b", foreground="#9aa8b8")
        style.configure("TButton", background="#2f6f73", foreground="#ffffff", borderwidth=0, padding=7)
        style.map("TButton", background=[("active", "#38858a"), ("disabled", "#3a4655")])
        style.configure("TCombobox", fieldbackground="#1c2430", background="#1c2430", foreground="#edf2f7")
        style.configure("Horizontal.TScale", background="#18202b", troughcolor="#0f141c")
        style.configure("Horizontal.TProgressbar", background="#46b3a7", troughcolor="#0f141c")

    def _build_variables(self):
        self.port_var = tk.StringVar()
        self.connection_var = tk.StringVar(value="Desconectado")
        self.link_var = tk.StringVar(value="Nano: ?")

        self.rgb_r = tk.IntVar(value=0)
        self.rgb_g = tk.IntVar(value=0)
        self.rgb_b = tk.IntVar(value=0)
        self.rgb_mode = tk.StringVar(value=RGB_MODE_LABELS["FIXED"])
        self.rgb_speed = tk.IntVar(value=100)

        self.wheel_raw = tk.IntVar(value=0)
        self.wheel_axis = tk.IntVar(value=0)
        self.wheel_center = tk.IntVar(value=2048)
        self.wheel_deadzone = tk.IntVar(value=45)
        self.wheel_sens = tk.IntVar(value=100)
        self.wheel_inv = tk.BooleanVar(value=True)

        self.pedal_raw = [tk.IntVar(value=0), tk.IntVar(value=0)]
        self.pedal_axis = [tk.IntVar(value=0), tk.IntVar(value=0)]
        self.pedal_min = [tk.IntVar(value=0), tk.IntVar(value=0)]
        self.pedal_max = [tk.IntVar(value=4095), tk.IntVar(value=4095)]
        self.pedal_sens = [tk.IntVar(value=100), tk.IntVar(value=100)]
        self.pedal_inv = [tk.BooleanVar(value=False), tk.BooleanVar(value=False)]

        self.motor_left = tk.IntVar(value=0)
        self.motor_right = tk.IntVar(value=0)
        self.motor_left_min = tk.IntVar(value=120)
        self.motor_right_min = tk.IntVar(value=100)
        self.motor_left_out = tk.IntVar(value=0)
        self.motor_right_out = tk.IntVar(value=0)

        self.wifi_ssid = tk.StringVar(value="")
        self.wifi_password = tk.StringVar(value="")
        self.ota_host = tk.StringVar(value="volante-s3")
        self.ota_password = tk.StringVar(value="")
        self.wifi_enabled = tk.StringVar(value="0")
        self.wifi_status = tk.StringVar(value="OFF")
        self.wifi_ip = tk.StringVar(value="-")
        self.wifi_rssi = tk.StringVar(value="0")
        self.ota_ready = tk.StringVar(value="0")
        self.ota_port = tk.StringVar(value="3232")
        self.enable_pov = tk.BooleanVar(value=True)
        self.rgb_active_low = tk.BooleanVar(value=False)
        self.pedals_pullup = tk.BooleanVar(value=True)

    def _build_ui(self):
        top = ttk.Frame(self, padding=12)
        top.pack(fill="x")

        ttk.Label(top, text="Puerto").pack(side="left")
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=36, state="readonly")
        self.port_combo.pack(side="left", padx=8)
        ttk.Button(top, text="Refrescar", command=self.refresh_ports).pack(side="left", padx=4)
        self.connect_button = ttk.Button(top, text="Conectar", command=self.toggle_connection)
        self.connect_button.pack(side="left", padx=4)
        ttk.Label(top, textvariable=self.connection_var).pack(side="left", padx=14)
        ttk.Label(top, textvariable=self.link_var).pack(side="left", padx=14)

        self.body_canvas = tk.Canvas(self, bg="#11151c", highlightthickness=0)
        self.body_scrollbar = ttk.Scrollbar(self, orient="vertical", command=self.body_canvas.yview)
        self.body_canvas.configure(yscrollcommand=self.body_scrollbar.set)
        self.body_canvas.pack(side="left", fill="both", expand=True)
        self.body_scrollbar.pack(side="right", fill="y")

        body = ttk.Frame(self.body_canvas, padding=(12, 0, 12, 12))
        self.body_window = self.body_canvas.create_window((0, 0), window=body, anchor="nw")
        body.bind("<Configure>", lambda event: self.body_canvas.configure(scrollregion=self.body_canvas.bbox("all")))
        self.body_canvas.bind("<Configure>", lambda event: self.body_canvas.itemconfigure(self.body_window, width=event.width))
        self.body_canvas.bind_all("<MouseWheel>", self._on_mousewheel)
        body.columnconfigure(0, weight=1)
        body.columnconfigure(1, weight=1)
        body.rowconfigure(2, weight=1)

        self._build_rgb_card(body).grid(row=0, column=0, sticky="nsew", padx=(0, 6), pady=6)
        self._build_motor_card(body).grid(row=0, column=1, sticky="nsew", padx=(6, 0), pady=6)
        self._build_wifi_card(body).grid(row=1, column=0, columnspan=2, sticky="nsew", pady=6)
        self._build_axes_card(body).grid(row=2, column=0, sticky="nsew", padx=(0, 6), pady=6)
        self._build_log_card(body).grid(row=2, column=1, sticky="nsew", padx=(6, 0), pady=6)

    def _on_mousewheel(self, event):
        self.body_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")

    def _card(self, parent, title):
        frame = ttk.Frame(parent, style="Card.TFrame", padding=12)
        ttk.Label(frame, text=title, style="Card.TLabel", font=("Segoe UI", 13, "bold")).pack(anchor="w")
        return frame

    def _build_rgb_card(self, parent):
        frame = self._card(parent, "RGB")

        self.color_preview = tk.Canvas(frame, width=120, height=42, highlightthickness=0, bg="#000000")
        self.color_preview.pack(anchor="w", pady=(10, 8))

        self._rgb_slider(frame, "Rojo", self.rgb_r)
        self._rgb_slider(frame, "Verde", self.rgb_g)
        self._rgb_slider(frame, "Azul", self.rgb_b)

        mode_row = ttk.Frame(frame, style="Card.TFrame")
        mode_row.pack(fill="x", pady=(8, 3))
        ttk.Label(mode_row, text="Modo", style="Card.TLabel", width=8).pack(side="left")
        self.rgb_mode_combo = ttk.Combobox(
            mode_row,
            textvariable=self.rgb_mode,
            values=list(RGB_MODE_COMMANDS.keys()),
            state="readonly",
        )
        self.rgb_mode_combo.pack(side="left", fill="x", expand=True, padx=8)

        speed_row = ttk.Frame(frame, style="Card.TFrame")
        speed_row.pack(fill="x", pady=3)
        ttk.Label(speed_row, text="Velocidad", style="Card.TLabel", width=8).pack(side="left")
        ttk.Scale(speed_row, from_=10, to=200, variable=self.rgb_speed).pack(side="left", fill="x", expand=True, padx=8)
        ttk.Label(speed_row, textvariable=self.rgb_speed, style="Card.TLabel", width=4).pack(side="left")

        row = ttk.Frame(frame, style="Card.TFrame")
        row.pack(fill="x", pady=(8, 0))
        ttk.Button(row, text="Aplicar iluminacion", command=self.apply_rgb).pack(side="left")
        ttk.Button(row, text="Apagar", command=self.set_rgb_off).pack(side="left", padx=6)
        ttk.Button(row, text="Azul", command=lambda: self.set_rgb_values(0, 0, 255, apply=True)).pack(side="left", padx=6)
        ttk.Button(row, text="Rojo", command=lambda: self.set_rgb_values(255, 0, 0, apply=True)).pack(side="left", padx=6)
        return frame

    def _rgb_slider(self, parent, label, var):
        row = ttk.Frame(parent, style="Card.TFrame")
        row.pack(fill="x", pady=3)
        ttk.Label(row, text=label, style="Card.TLabel", width=8).pack(side="left")
        scale = ttk.Scale(row, from_=0, to=255, variable=var, command=lambda _=None: self.update_preview())
        scale.pack(side="left", fill="x", expand=True, padx=8)
        ttk.Label(row, textvariable=var, style="Card.TLabel", width=4).pack(side="left")

    def _build_motor_card(self, parent):
        frame = self._card(parent, "Motores / test")
        self._motor_slider(frame, "Cmd izq GPIO7", self.motor_left)
        self._motor_slider(frame, "Cmd der GPIO6", self.motor_right)
        self._motor_slider(frame, "Min izq", self.motor_left_min)
        self._motor_slider(frame, "Min der", self.motor_right_min)
        out_row = ttk.Frame(frame, style="Card.TFrame")
        out_row.pack(fill="x", pady=(4, 0))
        ttk.Label(out_row, text="Salida real", style="Muted.TLabel").pack(side="left")
        ttk.Label(out_row, text="Izq", style="Card.TLabel").pack(side="left", padx=(10, 3))
        ttk.Label(out_row, textvariable=self.motor_left_out, style="Card.TLabel", width=4).pack(side="left")
        ttk.Label(out_row, text="Der", style="Card.TLabel").pack(side="left", padx=(10, 3))
        ttk.Label(out_row, textvariable=self.motor_right_out, style="Card.TLabel", width=4).pack(side="left")
        row = ttk.Frame(frame, style="Card.TFrame")
        row.pack(fill="x", pady=(8, 0))
        ttk.Button(row, text="Aplicar motor", command=self.apply_motor).pack(side="left")
        ttk.Button(row, text="Parar", command=self.stop_motor).pack(side="left", padx=6)
        ttk.Button(row, text="Aplicar punto muerto", command=self.apply_motor_min).pack(side="left", padx=6)
        ttk.Label(frame, text="Esto solo prueba PWM por Serial; no es force feedback de Steam todavia.",
                  style="Muted.TLabel").pack(anchor="w", pady=(10, 0))
        return frame

    def _build_wifi_card(self, parent):
        frame = self._card(parent, "WiFi / OTA")

        grid = ttk.Frame(frame, style="Card.TFrame")
        grid.pack(fill="x", pady=(10, 4))
        for col in range(6):
            grid.columnconfigure(col, weight=1 if col in (1, 3, 5) else 0)

        ttk.Label(grid, text="SSID", style="Card.TLabel").grid(row=0, column=0, sticky="w", padx=(0, 6), pady=3)
        ttk.Entry(grid, textvariable=self.wifi_ssid).grid(row=0, column=1, sticky="ew", padx=(0, 14), pady=3)
        ttk.Label(grid, text="Clave WiFi", style="Card.TLabel").grid(row=0, column=2, sticky="w", padx=(0, 6), pady=3)
        ttk.Entry(grid, textvariable=self.wifi_password, show="*").grid(row=0, column=3, sticky="ew", padx=(0, 14), pady=3)
        ttk.Button(grid, text="Guardar y conectar", command=self.apply_wifi).grid(row=0, column=4, sticky="ew", padx=(0, 6), pady=3)
        ttk.Button(grid, text="Apagar WiFi", command=lambda: self.send("WIFIOFF")).grid(row=0, column=5, sticky="ew", pady=3)

        ttk.Label(grid, text="Host OTA", style="Card.TLabel").grid(row=1, column=0, sticky="w", padx=(0, 6), pady=3)
        ttk.Entry(grid, textvariable=self.ota_host).grid(row=1, column=1, sticky="ew", padx=(0, 14), pady=3)
        ttk.Label(grid, text="Pass OTA", style="Card.TLabel").grid(row=1, column=2, sticky="w", padx=(0, 6), pady=3)
        ttk.Entry(grid, textvariable=self.ota_password, show="*").grid(row=1, column=3, sticky="ew", padx=(0, 14), pady=3)
        ttk.Button(grid, text="Aplicar OTA", command=self.apply_ota_settings).grid(row=1, column=4, sticky="ew", padx=(0, 6), pady=3)
        ttk.Button(grid, text="WIFI?", command=lambda: self.send("WIFI?")).grid(row=1, column=5, sticky="ew", pady=3)

        status = ttk.Frame(frame, style="Card.TFrame")
        status.pack(fill="x", pady=(4, 0))
        ttk.Label(status, text="Estado:", style="Muted.TLabel").pack(side="left")
        ttk.Label(status, textvariable=self.wifi_status, style="Card.TLabel", width=13).pack(side="left", padx=(4, 12))
        ttk.Label(status, text="IP:", style="Muted.TLabel").pack(side="left")
        ttk.Label(status, textvariable=self.wifi_ip, style="Card.TLabel", width=15).pack(side="left", padx=(4, 12))
        ttk.Label(status, text="RSSI:", style="Muted.TLabel").pack(side="left")
        ttk.Label(status, textvariable=self.wifi_rssi, style="Card.TLabel", width=5).pack(side="left", padx=(4, 12))
        ttk.Label(status, text="OTA:", style="Muted.TLabel").pack(side="left")
        ttk.Label(status, textvariable=self.ota_ready, style="Card.TLabel", width=3).pack(side="left", padx=(4, 12))
        ttk.Label(status, text="Puerto:", style="Muted.TLabel").pack(side="left")
        ttk.Label(status, textvariable=self.ota_port, style="Card.TLabel", width=5).pack(side="left", padx=(4, 12))
        self.ota_upload_button = ttk.Button(status, text="Subir update OTA", command=self.upload_ota_from_app)
        self.ota_upload_button.pack(side="left", padx=(12, 0))
        ttk.Label(status, text="No requiere BOOT si CONNECTED y OTA=1.",
                  style="Muted.TLabel").pack(side="left", padx=(12, 0))
        return frame

    def _motor_slider(self, parent, label, var):
        row = ttk.Frame(parent, style="Card.TFrame")
        row.pack(fill="x", pady=6)
        ttk.Label(row, text=label, style="Card.TLabel", width=10).pack(side="left")
        ttk.Scale(row, from_=0, to=255, variable=var).pack(side="left", fill="x", expand=True, padx=8)
        ttk.Label(row, textvariable=var, style="Card.TLabel", width=4).pack(side="left")

    def _build_axes_card(self, parent):
        frame = self._card(parent, "Calibracion y ejes")

        self._axis_block(frame, "Volante GPIO4", self.wheel_raw, self.wheel_axis)
        row = ttk.Frame(frame, style="Card.TFrame")
        row.pack(fill="x", pady=4)
        ttk.Button(row, text="Tomar centro", command=lambda: self.send("CAL WHEEL")).pack(side="left")
        ttk.Label(row, text="Centro", style="Card.TLabel").pack(side="left", padx=(10, 4))
        ttk.Label(row, textvariable=self.wheel_center, style="Card.TLabel", width=5).pack(side="left")
        ttk.Checkbutton(row, text="Invertir", variable=self.wheel_inv).pack(side="left", padx=10)

        self._setting_slider(frame, "Deadzone", self.wheel_deadzone, 0, 500)
        self._setting_slider(frame, "Sens volante", self.wheel_sens, 20, 200)
        ttk.Button(frame, text="Aplicar volante", command=self.apply_wheel_settings).pack(anchor="w", pady=(2, 10))

        options = ttk.Frame(frame, style="Card.TFrame")
        options.pack(fill="x", pady=(4, 8))
        ttk.Checkbutton(options, text="POV/HAT activo", variable=self.enable_pov).pack(side="left", padx=(0, 10))
        ttk.Checkbutton(options, text="RGB activo bajo", variable=self.rgb_active_low).pack(side="left", padx=10)
        ttk.Checkbutton(options, text="Pullup pedales", variable=self.pedals_pullup).pack(side="left", padx=10)
        ttk.Button(options, text="Aplicar opciones", command=self.apply_bool_options).pack(side="left", padx=10)

        self._pedal_block(frame, 0, "Pedal 1 GPIO2")
        self._pedal_block(frame, 1, "Pedal 2 GPIO3")
        return frame

    def _axis_block(self, parent, title, raw_var, axis_var):
        block = ttk.Frame(parent, style="Card.TFrame")
        block.pack(fill="x", pady=(10, 2))
        ttk.Label(block, text=title, style="Card.TLabel", font=("Segoe UI", 10, "bold")).pack(anchor="w")

        raw_row = ttk.Frame(block, style="Card.TFrame")
        raw_row.pack(fill="x", pady=2)
        ttk.Label(raw_row, text="Raw", style="Muted.TLabel", width=7).pack(side="left")
        raw_bar = ttk.Progressbar(raw_row, maximum=4095, variable=raw_var)
        raw_bar.pack(side="left", fill="x", expand=True, padx=8)
        ttk.Label(raw_row, textvariable=raw_var, style="Card.TLabel", width=5).pack(side="left")

        axis_progress = tk.IntVar(value=127)
        axis_var.trace_add("write", lambda *_: axis_progress.set(clamp(axis_var.get() + 127, 0, 254)))
        axis_row = ttk.Frame(block, style="Card.TFrame")
        axis_row.pack(fill="x", pady=2)
        ttk.Label(axis_row, text="Axis", style="Muted.TLabel", width=7).pack(side="left")
        ttk.Progressbar(axis_row, maximum=254, variable=axis_progress).pack(side="left", fill="x", expand=True, padx=8)
        ttk.Label(axis_row, textvariable=axis_var, style="Card.TLabel", width=5).pack(side="left")

    def _setting_slider(self, parent, label, var, low, high):
        row = ttk.Frame(parent, style="Card.TFrame")
        row.pack(fill="x", pady=3)
        ttk.Label(row, text=label, style="Card.TLabel", width=13).pack(side="left")
        ttk.Scale(row, from_=low, to=high, variable=var).pack(side="left", fill="x", expand=True, padx=8)
        ttk.Label(row, textvariable=var, style="Card.TLabel", width=5).pack(side="left")

    def _pedal_block(self, parent, idx, title):
        block = ttk.Frame(parent, style="Card.TFrame")
        block.pack(fill="x", pady=(10, 2))
        self._axis_block(block, title, self.pedal_raw[idx], self.pedal_axis[idx])

        row = ttk.Frame(block, style="Card.TFrame")
        row.pack(fill="x", pady=4)
        prefix = f"P{idx + 1}"
        ttk.Button(row, text="Tomar min", command=lambda p=prefix: self.send(f"CAL {p} MIN")).pack(side="left")
        ttk.Button(row, text="Tomar max", command=lambda p=prefix: self.send(f"CAL {p} MAX")).pack(side="left", padx=6)
        ttk.Label(row, text="Min", style="Card.TLabel").pack(side="left", padx=(10, 3))
        ttk.Label(row, textvariable=self.pedal_min[idx], style="Card.TLabel", width=5).pack(side="left")
        ttk.Label(row, text="Max", style="Card.TLabel").pack(side="left", padx=(10, 3))
        ttk.Label(row, textvariable=self.pedal_max[idx], style="Card.TLabel", width=5).pack(side="left")
        ttk.Checkbutton(row, text="Invertir", variable=self.pedal_inv[idx]).pack(side="left", padx=10)

        self._setting_slider(block, f"Sens {prefix}", self.pedal_sens[idx], 20, 200)
        ttk.Button(block, text=f"Aplicar {prefix}",
                   command=lambda p=prefix, i=idx: self.apply_pedal_settings(p, i)).pack(anchor="w", pady=(2, 0))

    def _build_log_card(self, parent):
        frame = self._card(parent, "Serial")
        self.log_text = tk.Text(frame, height=20, bg="#0b0f15", fg="#d5f0ef", insertbackground="#d5f0ef",
                                relief="flat", wrap="word")
        self.log_text.pack(fill="both", expand=True, pady=(10, 8))
        row = ttk.Frame(frame, style="Card.TFrame")
        row.pack(fill="x")
        ttk.Button(row, text="CFG?", command=lambda: self.send("CFG?")).pack(side="left")
        ttk.Button(row, text="STATUS?", command=lambda: self.send("STATUS?")).pack(side="left", padx=6)
        ttk.Button(row, text="Reset config", command=self.reset_config).pack(side="left", padx=6)
        ttk.Button(row, text="Buscar update app", command=lambda: self.check_for_app_updates(silent=False)).pack(side="left", padx=6)
        ttk.Button(row, text="Limpiar log", command=lambda: self.log_text.delete("1.0", "end")).pack(side="right")
        return frame

    def refresh_ports(self):
        if list_ports is None:
            return
        ports = list(list_ports.comports())
        self.port_values = [f"{p.device} - {p.description}" for p in ports]
        self.port_combo["values"] = self.port_values
        if self.port_values and not self.port_var.get():
            self.port_var.set(self.port_values[0])

    def selected_port(self):
        value = self.port_var.get()
        if not value:
            return ""
        return value.split(" - ", 1)[0]

    def toggle_connection(self):
        if self.serial_client:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        port = self.selected_port()
        if not port:
            messagebox.showwarning("Puerto", "Elegi un COM primero.")
            return
        try:
            self.serial_client = SerialClient(port, self.line_queue.put)
        except Exception as exc:
            messagebox.showerror("Serial", f"No pude abrir {port}:\n{exc}")
            self.serial_client = None
            return

        self.connection_var.set(f"Conectado a {port}")
        self.connect_button.configure(text="Desconectar")
        self.log(f"APP conectado a {port}")
        self.send("QUIET 1")
        self.send("CFG?")
        self.send("WIFI?")
        self.send("STATUS?")

    def disconnect(self):
        if self.serial_client:
            self.serial_client.close()
            self.serial_client = None
        self.connection_var.set("Desconectado")
        self.connect_button.configure(text="Conectar")
        self.log("APP desconectado")

    def send(self, command):
        if not self.serial_client:
            self.log(f"APP sin conexion: {command}")
            return
        try:
            self.serial_client.send(command)
            self.log(f"> {command}")
        except Exception as exc:
            self.log(f"APP ERROR enviando {command}: {exc}")

    def _drain_lines(self):
        while True:
            try:
                line = self.line_queue.get_nowait()
            except queue.Empty:
                break
            if line == "APP_OTA_DONE":
                self.ota_upload_running = False
                self.ota_upload_button.configure(state="normal")
                self.log("APP OTA finalizado")
                continue
            self.log(line)
            if line.startswith("CONFIG "):
                self.apply_config(parse_kv_line(line))
            elif line.startswith("STATUS "):
                self.apply_status(parse_kv_line(line))
            elif line.startswith("WIFI "):
                self.apply_wifi_status(parse_kv_line(line))
        self.after(100, self._drain_lines)

    def _poll_status(self):
        if self.serial_client:
            try:
                self.serial_client.send("STATUS?")
            except Exception as exc:
                self.log(f"APP ERROR status: {exc}")
        self.after(STATUS_INTERVAL_MS, self._poll_status)

    def log(self, line):
        self.log_text.insert("end", line + "\n")
        self.log_text.see("end")
        line_count = int(self.log_text.index("end-1c").split(".")[0])
        if line_count > 500:
            self.log_text.delete("1.0", "80.0")

    def apply_config(self, data):
        self.config.update(data)
        if "RGB" in data:
            rgb = self._parse_csv_ints(data["RGB"], 3)
            if rgb:
                self.set_rgb_values(*rgb, apply=False)
        if "RGB_MODE" in data:
            self.rgb_mode.set(RGB_MODE_LABELS.get(data["RGB_MODE"], self.rgb_mode.get()))
        self._set_int_if_present(self.rgb_speed, data, "RGB_SPEED")
        if data.get("WIFI_SSID") and data["WIFI_SSID"] != "-":
            self.wifi_ssid.set(data["WIFI_SSID"])
        if data.get("OTA_HOST") and data["OTA_HOST"] != "-":
            self.ota_host.set(data["OTA_HOST"])
        if "WIFI_EN" in data:
            self.wifi_enabled.set(data["WIFI_EN"])
        if "MMIN" in data:
            motor_min = self._parse_csv_ints(data["MMIN"], 2)
            if motor_min:
                self.motor_left_min.set(motor_min[0])
                self.motor_right_min.set(motor_min[1])
        self._set_bool_if_present(self.enable_pov, data, "POV")
        self._set_bool_if_present(self.rgb_active_low, data, "RGB_AL")
        self._set_bool_if_present(self.pedals_pullup, data, "PULLUP")
        self._set_int_if_present(self.wheel_center, data, "WC")
        self._set_int_if_present(self.wheel_deadzone, data, "WDZ")
        self._set_int_if_present(self.wheel_sens, data, "WSENS")
        self._set_bool_if_present(self.wheel_inv, data, "WINV")
        for idx in range(2):
            prefix = f"P{idx + 1}"
            self._set_int_if_present(self.pedal_min[idx], data, f"{prefix}MIN")
            self._set_int_if_present(self.pedal_max[idx], data, f"{prefix}MAX")
            self._set_int_if_present(self.pedal_sens[idx], data, f"{prefix}SENS")
            self._set_bool_if_present(self.pedal_inv[idx], data, f"{prefix}INV")

    def apply_status(self, data):
        self.status.update(data)
        self._set_int_if_present(self.wheel_raw, data, "WRAW")
        self._set_int_if_present(self.wheel_axis, data, "WAXIS")
        self._set_int_if_present(self.pedal_raw[0], data, "P1RAW")
        self._set_int_if_present(self.pedal_axis[0], data, "P1AXIS")
        self._set_int_if_present(self.pedal_raw[1], data, "P2RAW")
        self._set_int_if_present(self.pedal_axis[1], data, "P2AXIS")
        if "WIFI" in data:
            self.wifi_status.set(data["WIFI"])
        if "IP" in data:
            self.wifi_ip.set(data["IP"])
        if "RSSI" in data:
            self.wifi_rssi.set(data["RSSI"])
        if "OTA" in data:
            self.ota_ready.set(data["OTA"])
        if "MOTOR_OUT" in data:
            motor_out = self._parse_csv_ints(data["MOTOR_OUT"], 2)
            if motor_out:
                self.motor_left_out.set(motor_out[0])
                self.motor_right_out.set(motor_out[1])
        self.link_var.set(f"Nano: {data.get('LINK', '?')}")

    def apply_wifi_status(self, data):
        if "EN" in data:
            self.wifi_enabled.set(data["EN"])
        if data.get("SSID") and data["SSID"] != "-":
            self.wifi_ssid.set(data["SSID"])
        if "STATUS" in data:
            self.wifi_status.set(data["STATUS"])
        if "IP" in data:
            self.wifi_ip.set(data["IP"])
        if "RSSI" in data:
            self.wifi_rssi.set(data["RSSI"])
        if data.get("HOST") and data["HOST"] != "-":
            self.ota_host.set(data["HOST"])
        if "OTA" in data:
            self.ota_ready.set(data["OTA"])
        if "PORT" in data:
            self.ota_port.set(data["PORT"])

    def _set_int_if_present(self, var, data, key):
        if key not in data:
            return
        try:
            var.set(int(data[key]))
        except ValueError:
            pass

    def _set_bool_if_present(self, var, data, key):
        if key not in data:
            return
        var.set(data[key] not in ("0", "false", "False", "OFF"))

    def _parse_csv_ints(self, value, count):
        parts = value.split(",")
        if len(parts) != count:
            return None
        try:
            return [int(part) for part in parts]
        except ValueError:
            return None

    def update_preview(self):
        r = clamp(int(self.rgb_r.get()), 0, 255)
        g = clamp(int(self.rgb_g.get()), 0, 255)
        b = clamp(int(self.rgb_b.get()), 0, 255)
        self.color_preview.configure(bg=f"#{r:02x}{g:02x}{b:02x}")

    def set_rgb_values(self, r, g, b, apply=False):
        self.rgb_r.set(r)
        self.rgb_g.set(g)
        self.rgb_b.set(b)
        self.update_preview()
        if apply:
            self.rgb_mode.set(RGB_MODE_LABELS["FIXED"])
            self.apply_rgb()

    def set_rgb_off(self):
        self.rgb_mode.set(RGB_MODE_LABELS["OFF"])
        self.apply_rgb()

    def _validate_no_spaces(self, value, label):
        if any(ch.isspace() for ch in value):
            messagebox.showwarning("WiFi / OTA", f"{label} no puede tener espacios con este protocolo simple.")
            return False
        return True

    def _quote_serial_arg(self, value):
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'

    def apply_wifi(self):
        ssid = self.wifi_ssid.get().strip()
        password = self.wifi_password.get()
        if not ssid:
            messagebox.showwarning("WiFi", "SSID vacio.")
            return
        self.send(f"WIFI {self._quote_serial_arg(ssid)} {self._quote_serial_arg(password)}")

    def apply_ota_settings(self):
        host = self.ota_host.get().strip() or "volante-s3"
        password = self.ota_password.get().strip()
        if not self._validate_no_spaces(host, "Host OTA"):
            return
        if not self._validate_no_spaces(password, "Pass OTA"):
            return
        self.send(f"HOST {host}")
        self.send(f"OTAPASS {password if password else '-'}")

    def check_for_app_updates(self, silent=False):
        if self.update_check_running:
            return
        if not APP_UPDATE_REPO:
            if not silent:
                messagebox.showinfo(
                    "Updates",
                    "Todavia falta configurar APP_UPDATE_REPO en wheel_config_app.py con tu repo, por ejemplo: usuario/volante-config",
                )
            return

        self.update_check_running = True
        threading.Thread(target=self._update_check_worker, args=(silent,), daemon=True).start()

    def _update_check_worker(self, silent):
        try:
            latest, download_url, source = self._find_available_app_update()
            if latest and version_tuple(latest) > version_tuple(APP_VERSION):
                self.line_queue.put(f"APP UPDATE disponible {latest} (actual {APP_VERSION}) desde {source}")
                self.after(0, lambda latest=latest, url=download_url: self._prompt_update_download(latest, url))
            elif not silent:
                self.line_queue.put(f"APP UPDATE no hay updates. Version actual {APP_VERSION}")
                self.after(0, lambda: messagebox.showinfo("Updates", f"No hay updates. Version actual {APP_VERSION}"))
        except Exception as exc:
            if not silent:
                self.line_queue.put(f"APP UPDATE ERROR {exc}")
                self.after(0, lambda: messagebox.showerror("Updates", f"No pude comprobar updates:\n{exc}"))
        finally:
            self.update_check_running = False

    def _find_available_app_update(self):
        candidates = []
        errors = []

        try:
            latest, url = self._read_latest_release_update()
            if latest:
                candidates.append((latest, url, "GitHub Releases"))
        except Exception as exc:
            errors.append(f"release: {exc}")

        try:
            latest, url = self._read_branch_update()
            if latest:
                candidates.append((latest, url, "codigo remoto"))
        except Exception as exc:
            errors.append(f"rama: {exc}")

        if candidates:
            candidates.sort(key=lambda item: version_tuple(item[0]), reverse=True)
            return candidates[0]
        if errors:
            raise RuntimeError("; ".join(errors))
        return "", "", ""

    def _read_latest_release_update(self):
        url = f"https://api.github.com/repos/{APP_UPDATE_REPO}/releases/latest"
        release = json.loads(self._http_get_text(url))

        latest = release.get("tag_name", "").lstrip("vV")
        html_url = release.get("html_url", "")
        download_url = ""
        for asset in release.get("assets", []):
            name = asset.get("name", "").lower()
            if name.endswith((".zip", ".exe", ".msi")):
                download_url = asset.get("browser_download_url", "")
                break
        return latest, download_url or html_url

    def _read_branch_update(self):
        errors = []
        for branch in ("main", "master"):
            raw_url = f"https://raw.githubusercontent.com/{APP_UPDATE_REPO}/{branch}/wheel_config_app.py"
            try:
                source = self._http_get_text(raw_url)
            except Exception as exc:
                errors.append(f"{branch}: {exc}")
                continue

            latest = self._extract_remote_app_version(source)
            if latest:
                zip_url = f"https://github.com/{APP_UPDATE_REPO}/archive/refs/heads/{branch}.zip"
                return latest, zip_url

        if errors:
            raise RuntimeError("; ".join(errors))
        return "", ""

    def _http_get_text(self, url):
        request = urllib.request.Request(url, headers={"User-Agent": f"VolanteConfig/{APP_VERSION}"})
        with urllib.request.urlopen(request, timeout=8) as response:
            return response.read().decode("utf-8", errors="replace")

    def _extract_remote_app_version(self, source):
        for line in source.splitlines():
            stripped = line.strip()
            if not stripped.startswith("APP_VERSION") or "=" not in stripped:
                continue
            value = stripped.split("=", 1)[1].strip().strip("'\"")
            return value
        return ""

    def _prompt_update_download(self, latest, url):
        if not url:
            messagebox.showwarning("Updates", f"Hay version {latest}, pero no encontre asset para descargar.")
            return
        if not messagebox.askyesno("Updates", f"Hay una version nueva: {latest}\n\nDescargarla ahora?"):
            return

        if not url.lower().split("?", 1)[0].endswith(".zip"):
            webbrowser.open(url)
            return

        threading.Thread(target=self._download_app_update_worker, args=(latest, url), daemon=True).start()

    def _download_app_update_worker(self, latest, url):
        try:
            root = Path(__file__).resolve().parent
            update_dir = root / APP_UPDATE_DIR
            update_dir.mkdir(parents=True, exist_ok=True)
            zip_path = update_dir / f"volante-config-{latest}.zip"

            self.line_queue.put(f"APP UPDATE descargando {latest}")
            request = urllib.request.Request(url, headers={"User-Agent": f"VolanteConfig/{APP_VERSION}"})
            with urllib.request.urlopen(request, timeout=30) as response, zip_path.open("wb") as output:
                while True:
                    chunk = response.read(1024 * 64)
                    if not chunk:
                        break
                    output.write(chunk)

            marker = {
                "version": latest,
                "zip": str(zip_path),
                "url": url,
            }
            (root / APP_UPDATE_MARKER).write_text(json.dumps(marker, indent=2), encoding="utf-8")
            self.line_queue.put("APP UPDATE descargada. Se aplicara en el proximo inicio.")
            self.after(
                0,
                lambda: messagebox.showinfo(
                    "Updates",
                    "Update descargada.\n\nCierra y abre Volante Config: el inicio siguiente reemplaza los archivos y abre la version nueva.",
                ),
            )
        except Exception as exc:
            self.line_queue.put(f"APP UPDATE ERROR descargando: {exc}")
            self.after(0, lambda exc=exc: messagebox.showerror("Updates", f"No pude descargar la update:\n{exc}"))

    def apply_bool_options(self):
        self.send(f"BOOL POV {1 if self.enable_pov.get() else 0}")
        self.send(f"BOOL RGBAL {1 if self.rgb_active_low.get() else 0}")
        self.send(f"BOOL PULLUP {1 if self.pedals_pullup.get() else 0}")

    def upload_ota_from_app(self):
        if self.ota_upload_running:
            messagebox.showinfo("OTA", "Ya hay una subida OTA en curso.")
            return

        ip = self.wifi_ip.get().strip()
        if not ip or ip == "-":
            messagebox.showwarning("OTA", "No hay IP WiFi. Conecta el ESP al WiFi primero.")
            return
        if self.wifi_status.get() != "CONNECTED" or self.ota_ready.get() != "1":
            messagebox.showwarning("OTA", "OTA no esta listo. Espera STATUS=CONNECTED y OTA=1.")
            return

        password = self.ota_password.get().strip()
        if not self._validate_no_spaces(password, "Pass OTA"):
            return

        if not messagebox.askyesno("OTA", f"Compilar y subir firmware por OTA a {ip}?\n\nNo toques BOOT ni reset mientras sube."):
            return

        self.ota_upload_running = True
        self.ota_upload_button.configure(state="disabled")
        thread = threading.Thread(target=self._run_ota_upload_worker, args=(ip, password), daemon=True)
        thread.start()

    def _run_ota_upload_worker(self, ip, password):
        root = Path(__file__).resolve().parent
        ota_dir = root / "ota"
        script = ota_dir / "upload_ota.ps1"
        if not script.exists():
            self.line_queue.put(f"APP OTA ERROR no existe {script}")
            self.line_queue.put("APP_OTA_DONE")
            return

        cmd = [
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
            "-Ip",
            ip,
        ]
        if password:
            cmd.extend(["-Password", password])

        self.line_queue.put(f"APP OTA iniciando upload a {ip}")
        try:
            process = subprocess.Popen(
                cmd,
                cwd=str(ota_dir),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            assert process.stdout is not None
            for line in process.stdout:
                line = line.rstrip()
                if line:
                    self.line_queue.put(f"OTA {line}")
            return_code = process.wait()
            if return_code == 0:
                self.line_queue.put("APP OTA OK. El ESP se va a reiniciar.")
            else:
                self.line_queue.put(f"APP OTA ERROR return_code={return_code}")
        except Exception as exc:
            self.line_queue.put(f"APP OTA ERROR {exc}")
        finally:
            self.line_queue.put("APP_OTA_DONE")

    def apply_rgb(self):
        mode = RGB_MODE_COMMANDS.get(self.rgb_mode.get(), "FIXED")
        self.send(f"RGB {int(self.rgb_r.get())} {int(self.rgb_g.get())} {int(self.rgb_b.get())}")
        self.send(f"RGBMODE {mode}")
        self.send(f"RGBSPEED {int(self.rgb_speed.get())}")

    def apply_motor(self):
        self.send(f"M {int(self.motor_left.get())} {int(self.motor_right.get())}")

    def apply_motor_min(self):
        self.send(f"MMIN {int(self.motor_left_min.get())} {int(self.motor_right_min.get())}")

    def stop_motor(self):
        self.motor_left.set(0)
        self.motor_right.set(0)
        self.apply_motor()

    def apply_wheel_settings(self):
        self.send(f"DEADZONE {int(self.wheel_deadzone.get())}")
        self.send(f"SENS W {int(self.wheel_sens.get())}")
        self.send(f"INV W {1 if self.wheel_inv.get() else 0}")

    def apply_pedal_settings(self, pedal, idx):
        self.send(f"SENS {pedal} {int(self.pedal_sens[idx].get())}")
        self.send(f"INV {pedal} {1 if self.pedal_inv[idx].get() else 0}")

    def reset_config(self):
        if messagebox.askyesno("Reset config", "Resetear RGB/calibracion/sensibilidad guardados?"):
            self.send("RESETCFG")

    def on_close(self):
        self.disconnect()
        self.destroy()


def apply_pending_update_if_needed():
    root = Path(__file__).resolve().parent
    marker_path = root / APP_UPDATE_MARKER
    if not marker_path.exists():
        return False

    try:
        marker = json.loads(marker_path.read_text(encoding="utf-8"))
        zip_path = Path(marker.get("zip", ""))
        version = marker.get("version", "?")
    except Exception:
        marker_path.unlink(missing_ok=True)
        return False

    if not zip_path.exists():
        marker_path.unlink(missing_ok=True)
        return False

    updater_dir = Path(tempfile.gettempdir()) / "VolanteConfigUpdate"
    updater_dir.mkdir(parents=True, exist_ok=True)
    ps_path = updater_dir / "apply_update.ps1"
    bat_path = updater_dir / "apply_update.bat"

    ps_path.write_text(
        """param(
  [Parameter(Mandatory = $true)]
  [string]$Root,

  [Parameter(Mandatory = $true)]
  [string]$Zip,

  [string]$Version = ""
)

$ErrorActionPreference = "Stop"
Start-Sleep -Seconds 2

$extractDir = Join-Path ([System.IO.Path]::GetTempPath()) "VolanteConfigUpdateExtract"
if (Test-Path $extractDir) {
  Remove-Item -LiteralPath $extractDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

Expand-Archive -LiteralPath $Zip -DestinationPath $extractDir -Force
$source = Get-ChildItem -Path $extractDir -Directory | Select-Object -First 1
if ($null -eq $source) {
  throw "ZIP invalido: no encontre carpeta fuente."
}

$obsolete = @("install_windows.bat", "instalar_volante_config.bat", "upload_ota.ps1")
foreach ($name in $obsolete) {
  $path = Join-Path $Root $name
  if (Test-Path $path) {
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
  }
}

Copy-Item -Path (Join-Path $source.FullName "*") -Destination $Root -Recurse -Force
Remove-Item -LiteralPath (Join-Path $Root ".pending_update.json") -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue

$launcher = Join-Path $Root "run_wheel_config_app.bat"
if (Test-Path $launcher) {
  Start-Process -FilePath $launcher -WorkingDirectory $Root
} else {
  Start-Process -FilePath "python" -ArgumentList (Join-Path $Root "wheel_config_app.py") -WorkingDirectory $Root
}
""",
        encoding="utf-8",
    )

    bat_path.write_text(
        f"""@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "{ps_path}" -Root "{root}" -Zip "{zip_path}" -Version "{version}"
del /f /q "{ps_path}" >nul 2>nul
del /f /q "%~f0" >nul 2>nul
""",
        encoding="utf-8",
    )

    subprocess.Popen(["cmd", "/c", str(bat_path)], cwd=str(root))
    return True


def main():
    if apply_pending_update_if_needed():
        return 0

    if serial is None:
        print("Falta pyserial. Instala con: python -m pip install -r requirements.txt")
        messagebox.showerror("Dependencia faltante", "Falta pyserial.\n\nEjecuta:\npython -m pip install -r requirements.txt")
        return 1

    app = WheelConfigApp()
    app.protocol("WM_DELETE_WINDOW", app.on_close)
    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
