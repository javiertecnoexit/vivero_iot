#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fake_backend.py - Backend de prueba para Vivero IoT (M6)

Simula el minimo contrato de docs/02_protocolo_ws.md:

  POST /api/v1/devices/claim   -> token + ws_url + config_version
  WS   /ws/device              -> hello/welcome/config/command/request/ping/pong
  Consola interactiva          -> state, config, on/off, open/close, raw, ...

No es un backend de produccion: no usa base de datos, no valida usuarios y
guarda todo en memoria. Su objetivo es probar el firmware en banco sin depender
del equipo de backend.

Uso:
  python tools/fake_backend.py
  python tools/fake_backend.py --http-port 8080 --ws-port 8081
  python tools/fake_backend.py --selftest

Requisitos:
  Python 3.10+ y websockets >= 14  (pip install "websockets>=14")
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import datetime as dt
import functools
import json
import os
import secrets
import shlex
import socket
import sys
import threading
import time
import traceback
import urllib.error
import urllib.request
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

try:
    from websockets.asyncio.client import connect
    from websockets.asyncio.server import ServerConnection, serve
    from websockets.exceptions import ConnectionClosed
except ImportError:  # pragma: no cover - mensaje para el operador
    print(
        "ERROR: falta la libreria 'websockets'. Instalar con:\n"
        '  python -m pip install "websockets>=14"',
        file=sys.stderr,
    )
    raise SystemExit(2)


# ---------------------------------------------------------------------------
# Constantes y utilidades
# ---------------------------------------------------------------------------

PROTOCOL_VERSION = 1
CLAIM_PATH = "/api/v1/devices/claim"
WS_PATH = "/ws/device"
MAX_FRAME_BYTES = 16384

# Canales que acepta el firmware. Se usan para validar la consola.
CHANNELS = (
    "riego_z1",
    "riego_z2",
    "aspersion",
    "cama_caliente",
    "iluminacion",
    "ventilacion",
    "ventana",
    "alarma",
)

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def utc_now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc)


def iso_now() -> str:
    return utc_now().strftime("%Y-%m-%dT%H:%M:%SZ")


def iso_after(seconds: float) -> str:
    return (utc_now() + dt.timedelta(seconds=seconds)).strftime("%Y-%m-%dT%H:%M:%SZ")


def compact(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def log(tag: str, message: str) -> None:
    print(f"{dt.datetime.now().strftime('%H:%M:%S')} {tag:<6} {message}", flush=True)


def local_ipv4_addresses() -> list[str]:
    """Direcciones IPv4 utiles para mostrar en el banner."""
    found: list[str] = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in found and not ip.startswith("127."):
                found.append(ip)
    except OSError:
        pass
    # El truco del socket UDP no envia paquetes pero descubre la interfaz
    # preferida (util cuando el hostname no resuelve a la LAN).
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        sock.close()
        if ip not in found and not ip.startswith("127."):
            found.insert(0, ip)
    except OSError:
        pass
    return found


def looks_like_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


# ---------------------------------------------------------------------------
# Configuracion de ejemplo
# ---------------------------------------------------------------------------


def default_config_payload(version: int) -> dict[str, Any]:
    """Config completa y valida para el parser de ws_client.cpp."""
    return {
        "config_version": version,
        "timezone_offset_min": -180,
        "irrigation": {
            "interlock": True,
            "stagger_s": 30,
        },
        "channels": {
            "riego_z1": {
                "auto_enabled": True,
                "schedule": {
                    "enabled": True,
                    "entries": [
                        {
                            "id": "z1-1",
                            "start": "07:00",
                            "duration_s": 600,
                            "days": [1, 2, 3, 4, 5, 6, 7],
                        }
                    ],
                },
                "moisture": {
                    "enabled": True,
                    "start_below_pct": 30.0,
                    "stop_above_pct": 45.0,
                    "max_run_s": 900,
                    "min_off_s": 1800,
                    "window_start": "06:00",
                    "window_end": "20:00",
                },
            },
            "riego_z2": {
                "auto_enabled": True,
                "schedule": {
                    "enabled": True,
                    "entries": [
                        {
                            "id": "z2-1",
                            "start": "07:30",
                            "duration_s": 600,
                            "days": [1, 3, 5],
                        }
                    ],
                },
                "moisture": {
                    "enabled": False,
                    "start_below_pct": 30.0,
                    "stop_above_pct": 45.0,
                    "max_run_s": 900,
                    "min_off_s": 1800,
                    "window_start": "06:00",
                    "window_end": "20:00",
                },
            },
            "aspersion": {
                "auto_enabled": True,
                "max_run_s": 1800,
                "schedule": {
                    "enabled": True,
                    "entries": [
                        {
                            "id": "asp-1",
                            "start": "18:00",
                            "duration_s": 300,
                            "days": [1, 2, 3, 4, 5, 6, 7],
                        }
                    ],
                },
            },
            "cama_caliente": {
                "auto_enabled": True,
                "thermostat": {
                    "setpoint_c": 25.0,
                    "hysteresis_c": 1.0,
                    "max_c": 35.0,
                    "sensor": "bed",
                },
            },
            "iluminacion": {
                "auto_enabled": True,
                "max_run_s": 57600,
                "schedule": {
                    "enabled": True,
                    "entries": [
                        {
                            "id": "luz-1",
                            "start": "06:00",
                            "duration_s": 43200,
                            "days": [1, 2, 3, 4, 5, 6, 7],
                        }
                    ],
                },
            },
            "ventilacion": {
                "auto_enabled": True,
                "schedule": {"enabled": False, "entries": []},
                "climate": {
                    "enabled": True,
                    "on_above_temp_c": 30.0,
                    "off_below_temp_c": 27.0,
                    "on_above_rh_pct": 85.0,
                    "off_below_rh_pct": 75.0,
                    "min_on_s": 60,
                    "min_off_s": 120,
                    "max_run_s": 3600,
                },
            },
            "ventana": {
                "auto_enabled": True,
                "pulse_ms": 5000,
                "travel_time_s": 20,
                "max_travel_s": 30,
                "min_interval_s": 60,
            },
            "alarma": {"auto_enabled": True},
        },
        "alarms": {
            "enabled": True,
            "silence_max_s": 600,
            "rules": [
                {
                    "id": "air_temp_high",
                    "enabled": True,
                    "metric": "air_temp_c",
                    "op": ">",
                    "threshold": 38.0,
                    "hysteresis": 1.0,
                    "delay_s": 60,
                    "severity": "critical",
                    "latch": False,
                },
                {
                    "id": "air_temp_low",
                    "enabled": True,
                    "metric": "air_temp_c",
                    "op": "<",
                    "threshold": 5.0,
                    "hysteresis": 1.0,
                    "delay_s": 60,
                    "severity": "warning",
                    "latch": False,
                },
                {
                    "id": "bed_temp_high",
                    "enabled": True,
                    "metric": "bed_temp_c",
                    "op": ">",
                    "threshold": 35.0,
                    "hysteresis": 1.0,
                    "delay_s": 30,
                    "severity": "critical",
                    "latch": False,
                },
                {
                    "id": "sensor_fault",
                    "enabled": True,
                    "metric": "sensor_fault_count",
                    "op": ">=",
                    "threshold": 1,
                    "hysteresis": 0,
                    "delay_s": 120,
                    "severity": "warning",
                    "latch": False,
                },
                {
                    "id": "window_timeout",
                    "enabled": True,
                    "metric": "window_timeout",
                    "op": "==",
                    "threshold": 1,
                    "hysteresis": 0,
                    "delay_s": 0,
                    "severity": "warning",
                    "latch": True,
                },
                {
                    "id": "offline",
                    "enabled": True,
                    "metric": "offline_s",
                    "op": ">",
                    "threshold": 600,
                    "hysteresis": 0,
                    "delay_s": 0,
                    "severity": "warning",
                    "latch": False,
                },
            ],
        },
    }


# ---------------------------------------------------------------------------
# Sesion de un dispositivo
# ---------------------------------------------------------------------------


class DeviceSession:
    def __init__(self, ws: ServerConnection, device_id: str):
        self.ws = ws
        self.device_id = device_id
        self.boot_id: str | None = None
        self.session_id = f"s-{secrets.token_hex(3)}"
        self.hello: dict[str, Any] = {}
        self.connected_at = time.monotonic()
        self.send_lock = asyncio.Lock()

    async def send(self, message: dict[str, Any]) -> None:
        text = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
        async with self.send_lock:
            await self.ws.send(text)

    def elapsed_s(self) -> float:
        return time.monotonic() - self.connected_at


# ---------------------------------------------------------------------------
# Backend
# ---------------------------------------------------------------------------


class FakeBackend:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.token = args.token
        self.claim_code = args.claim_code
        self.config_version = int(args.config_version)
        self.device: DeviceSession | None = None
        self.server_seq = 0
        self.command_seq = 0
        self.stats: Counter[str] = Counter()
        self.stop_event: asyncio.Event | None = None
        self.ws_port = int(args.ws_port)
        self.public_ws_url = args.public_ws_url
        self._csv_file = None
        self._csv_writer = None
        if args.csv:
            self._open_csv(args.csv)

    # -- utilidades ---------------------------------------------------------

    def next_id(self, prefix: str) -> str:
        self.server_seq += 1
        return f"{prefix}-{self.server_seq:04d}"

    def envelope(
        self,
        msg_type: str,
        payload: dict[str, Any],
        msg_id: str | None = None,
    ) -> dict[str, Any]:
        return {
            "v": PROTOCOL_VERSION,
            "type": msg_type,
            "id": msg_id or self.next_id("srv"),
            "ts": iso_now(),
            "payload": payload,
        }

    def ws_url_for(self, host_header: str) -> str:
        if self.public_ws_url:
            return self.public_ws_url
        host = self.args.public_ws_host or ""
        if not host:
            value = (host_header or "").strip()
            if value.startswith("["):
                end = value.find("]")
                host = value[1:end] if end > 0 else value
            elif value:
                host = value.rsplit(":", 1)[0]
            else:
                host = "127.0.0.1"
        if ":" in host and not host.startswith("["):
            host = f"[{host}]"
        return f"ws://{host}:{self.ws_port}{WS_PATH}"

    def _open_csv(self, path_text: str) -> None:
        path = Path(path_text)
        if path.parent and str(path.parent) not in ("", "."):
            path.parent.mkdir(parents=True, exist_ok=True)
        self._csv_file = path.open("w", newline="", encoding="utf-8")
        self._csv_writer = csv.DictWriter(
            self._csv_file,
            fieldnames=[
                "received_at",
                "device_id",
                "boot_id",
                "seq",
                "ts",
                "buffered",
                "air_temp_c",
                "air_rh_pct",
                "air_valid",
                "soil1_moisture_pct",
                "soil1_temp_c",
                "soil1_valid",
                "soil2_moisture_pct",
                "soil2_temp_c",
                "soil2_valid",
                "bed_temp_c",
                "bed_valid",
                "window_state",
                "rssi_dbm",
                "uptime_s",
                "time_valid",
            ],
        )
        self._csv_writer.writeheader()
        log("SYS", f"CSV de telemetria: {path.resolve()}")

    def close(self) -> None:
        if self._csv_file is not None:
            try:
                self._csv_file.flush()
                self._csv_file.close()
            except OSError:
                pass
            self._csv_file = None
            self._csv_writer = None

    def _write_csv_sample(
        self, session: DeviceSession, seq: Any, buffered: Any, sample: dict[str, Any]
    ) -> None:
        if self._csv_writer is None:
            return
        air = sample.get("air") or {}
        substrate = sample.get("substrate") or {}
        z1 = substrate.get("z1") or {}
        z2 = substrate.get("z2") or {}
        bed = sample.get("bed") or {}
        window = sample.get("window") or {}
        row = {
            "received_at": iso_now(),
            "device_id": session.device_id,
            "boot_id": session.boot_id or "",
            "seq": seq,
            "ts": sample.get("ts") or "",
            "buffered": buffered,
            "air_temp_c": air.get("temp_c"),
            "air_rh_pct": air.get("rh_pct"),
            "air_valid": air.get("valid"),
            "soil1_moisture_pct": z1.get("moisture_pct"),
            "soil1_temp_c": z1.get("temp_c"),
            "soil1_valid": z1.get("valid"),
            "soil2_moisture_pct": z2.get("moisture_pct"),
            "soil2_temp_c": z2.get("temp_c"),
            "soil2_valid": z2.get("valid"),
            "bed_temp_c": bed.get("temp_c"),
            "bed_valid": bed.get("valid"),
            "window_state": window.get("state"),
            "rssi_dbm": sample.get("rssi_dbm"),
            "uptime_s": sample.get("uptime_s"),
            "time_valid": sample.get("time_valid"),
        }
        self._csv_writer.writerow(row)

    # -- envio --------------------------------------------------------------

    async def send_welcome(self, session: DeviceSession) -> None:
        payload = {
            "session_id": session.session_id,
            "server_time": iso_now(),
            "heartbeat_s": max(5, int(self.args.heartbeat)),
            "telemetry_s": max(10, int(self.args.telemetry)),
            "config_version": self.config_version,
            "send_config": bool(self.args.auto_config),
            "limits": {"max_frame_bytes": MAX_FRAME_BYTES, "max_commands_per_min": 30},
        }
        await session.send(self.envelope("welcome", payload, self.next_id("srv-welcome")))
        log(
            "SRV>",
            "welcome session=%s heartbeat=%ss telemetry=%ss cfg=%s send_config=%s"
            % (
                session.session_id,
                payload["heartbeat_s"],
                payload["telemetry_s"],
                self.config_version,
                payload["send_config"],
            ),
        )

    async def send_config(self, session: DeviceSession, bump: bool = True) -> None:
        if bump:
            self.config_version += 1
        payload = default_config_payload(self.config_version)
        msg_id = f"cfg-{self.config_version:04d}"
        await session.send(self.envelope("config", payload, msg_id))
        log("SRV>", f"config id={msg_id} version={self.config_version}")

    async def send_ping(self, session: DeviceSession) -> None:
        msg = self.envelope(
            "ping", {"server_time": iso_now()}, self.next_id("srv-ping")
        )
        await session.send(msg)
        log("SRV>", f"ping id={msg['id']}")

    async def send_request(self, session: DeviceSession, what: str) -> None:
        msg = self.envelope(
            "request", {"what": what}, self.next_id("srv-req")
        )
        await session.send(msg)
        log("SRV>", f"request what={what} id={msg['id']}")

    async def send_command(
        self,
        session: DeviceSession,
        channel: str | None,
        action: str,
        duration_s: int | None = None,
        **extra: Any,
    ) -> str:
        self.command_seq += 1
        msg_id = f"cmd-{self.command_seq:04x}"
        payload: dict[str, Any] = {
            "action": action,
            "requested_by": "fake_backend",
            "reason": "manual",
        }
        if channel:
            payload["channel"] = channel
        if action != "report_state":
            payload["expires_at"] = iso_after(float(self.args.expire_s))
        if duration_s is not None:
            payload["duration_s"] = int(duration_s)
        payload.update(extra)
        await session.send(self.envelope("command", payload, msg_id))
        log(
            "SRV>",
            "command id=%s channel=%s action=%s duration=%s expires=%s"
            % (
                msg_id,
                channel or "-",
                action,
                duration_s if duration_s is not None else "-",
                payload.get("expires_at", "-"),
            ),
        )
        return msg_id

    async def send_raw(self, session: DeviceSession, raw: str) -> None:
        try:
            message = json.loads(raw)
        except json.JSONDecodeError as exc:
            log("SYS", f"raw: JSON invalido: {exc}")
            return
        if not isinstance(message, dict):
            log("SYS", "raw: se esperaba un objeto JSON")
            return
        message.setdefault("v", PROTOCOL_VERSION)
        message.setdefault("id", self.next_id("raw"))
        message.setdefault("ts", iso_now())
        message.setdefault("payload", {})
        await session.send(message)
        log("SRV>", f"raw {message.get('type')} id={message.get('id')}")

    # -- recepcion ----------------------------------------------------------

    async def on_hello(self, session: DeviceSession, payload: dict[str, Any]) -> None:
        self.stats["hello"] += 1
        session.hello = payload
        session.boot_id = payload.get("boot_id")
        hello_cfg = payload.get("config_version")
        if looks_like_int(hello_cfg) and hello_cfg >= 0:
            self.config_version = max(self.config_version, int(hello_cfg) + 1)

        caps = payload.get("capabilities") or {}
        enabled = caps.get("enabled_channels") or []
        log(
            "DEV>",
            "hello device=%s fw=%s mode=%s cfg=%s local=%s source=%s "
            "time_valid=%s ip=%s rssi=%s enabled=%s"
            % (
                payload.get("device_id", "-"),
                payload.get("fw_version", "-"),
                payload.get("mode", "-"),
                hello_cfg,
                payload.get("local_version", "-"),
                payload.get("config_source", "-"),
                payload.get("time_valid", "-"),
                payload.get("ip", "-"),
                payload.get("rssi_dbm", "-"),
                ",".join(enabled) if enabled else "-",
            ),
        )
        if payload.get("device_id") and payload["device_id"] != session.device_id:
            log(
                "SYS",
                "aviso: X-Device-Id=%s distinto de hello.device_id=%s"
                % (session.device_id, payload["device_id"]),
            )

        await self.send_welcome(session)
        if self.args.auto_config:
            await asyncio.sleep(0.15)
            await self.send_config(session, bump=False)

    def on_telemetry(self, session: DeviceSession, payload: dict[str, Any]) -> None:
        self.stats["telemetry"] += 1
        samples = payload.get("samples") or []
        seq = payload.get("seq")
        buffered = payload.get("buffered")
        log(
            "DEV>",
            "telemetry boot=%s seq=%s samples=%s buffered=%s"
            % (
                payload.get("boot_id", session.boot_id or "-"),
                seq,
                len(samples),
                buffered,
            ),
        )
        for sample in samples:
            if not isinstance(sample, dict):
                continue
            air = sample.get("air") or {}
            substrate = sample.get("substrate") or {}
            z1 = substrate.get("z1") or {}
            z2 = substrate.get("z2") or {}
            bed = sample.get("bed") or {}
            window = sample.get("window") or {}
            log(
                "      ",
                "ts=%s air=%sC/%s%% soil1=%s%%/%sC soil2=%s%%/%sC "
                "bed=%sC win=%s rssi=%s"
                % (
                    sample.get("ts", "-"),
                    air.get("temp_c", "-"),
                    air.get("rh_pct", "-"),
                    z1.get("moisture_pct", "-"),
                    z1.get("temp_c", "-"),
                    z2.get("moisture_pct", "-"),
                    z2.get("temp_c", "-"),
                    bed.get("temp_c", "-"),
                    window.get("state", "-"),
                    sample.get("rssi_dbm", "-"),
                ),
            )
            self._write_csv_sample(session, seq, buffered, sample)
        if self.args.verbose:
            log("      ", compact(payload))

    def on_state(self, session: DeviceSession, payload: dict[str, Any]) -> None:
        self.stats["state"] += 1
        channels = payload.get("channels") or {}
        on_channels = [
            name for name, value in channels.items() if (value or {}).get("on")
        ]
        alarms = (payload.get("alarms") or {}).get("active") or []
        window = payload.get("window") or {}
        clock = payload.get("clock") or {}
        wifi = payload.get("wifi") or {}
        ws = payload.get("ws") or {}
        log(
            "DEV>",
            "state mode=%s cfg=%s local=%s source=%s clock=%s wifi=%s ws=%s "
            "on=%s alarms=%s window=%s"
            % (
                payload.get("mode", "-"),
                payload.get("config_version", "-"),
                payload.get("local_version", "-"),
                payload.get("config_source", "-"),
                clock.get("valid", "-"),
                wifi.get("connected", "-"),
                ws.get("connected", "-"),
                ",".join(on_channels) if on_channels else "-",
                ",".join(alarms) if alarms else "-",
                window.get("state", "-"),
            ),
        )
        if self.args.verbose:
            log("      ", compact(payload))

    def on_event(self, session: DeviceSession, payload: dict[str, Any]) -> None:
        self.stats["event"] += 1
        log(
            "DEV>",
            "event %s sev=%s ch=%s buffered=%s msg=%s data=%s"
            % (
                payload.get("code", "-"),
                payload.get("severity", "-"),
                payload.get("channel") or "-",
                payload.get("buffered", "-"),
                payload.get("message", "-"),
                compact(payload.get("data") or {}),
            ),
        )

    def on_ack(self, session: DeviceSession, message: dict[str, Any]) -> None:
        self.stats["ack"] += 1
        payload = message.get("payload") or {}
        ref = payload.get("ref", "-")
        status = payload.get("status", "-")
        reason = payload.get("reason")
        applied = payload.get("applied") or {}
        warnings = payload.get("warnings") or []
        extra = ""
        if str(ref).startswith("cfg-"):
            extra = " stored=%s applied=%s source=%s version=%s" % (
                payload.get("stored", "-"),
                payload.get("applied", "-"),
                payload.get("active_source", "-"),
                payload.get("config_version", "-"),
            )
        log(
            "DEV>",
            "ack ref=%s status=%s%s%s"
            % (ref, status, f" reason={reason}" if reason else "", extra),
        )
        if warnings:
            log("      ", f"warnings={json.dumps(warnings, ensure_ascii=False)}")
        if applied:
            log("      ", f"applied={compact(applied)}")

    def on_pong(self, session: DeviceSession, payload: dict[str, Any]) -> None:
        self.stats["pong"] += 1
        log(
            "DEV>",
            "pong echo=%s uptime=%ss"
            % (payload.get("server_time_echo", "-"), payload.get("uptime_s", "-")),
        )

    async def on_device_message(self, session: DeviceSession, raw: str) -> None:
        if self.args.verbose:
            log("DEV>", raw)
        try:
            message = json.loads(raw)
        except json.JSONDecodeError as exc:
            log("SYS", f"JSON invalido del dispositivo: {exc}")
            await session.send(
                self.envelope(
                    "error",
                    {
                        "code": "invalid_json",
                        "message": "frame no parseable",
                        "ref": None,
                        "fatal": False,
                    },
                )
            )
            return
        if not isinstance(message, dict):
            log("SYS", "el frame no es un objeto JSON")
            return

        version = message.get("v")
        if version != PROTOCOL_VERSION:
            await session.send(
                self.envelope(
                    "error",
                    {
                        "code": "unsupported_version",
                        "message": f"version {version} no soportada",
                        "ref": message.get("id"),
                        "fatal": False,
                    },
                )
            )
            return

        msg_type = message.get("type")
        payload = message.get("payload")
        if not isinstance(payload, dict):
            payload = {}

        if msg_type == "hello":
            await self.on_hello(session, payload)
        elif msg_type == "telemetry":
            self.on_telemetry(session, payload)
        elif msg_type == "state":
            self.on_state(session, payload)
        elif msg_type == "event":
            self.on_event(session, payload)
        elif msg_type == "ack":
            self.on_ack(session, message)
        elif msg_type == "pong":
            self.on_pong(session, payload)
        elif msg_type == "error":
            log(
                "DEV>",
                "error code=%s message=%s ref=%s"
                % (
                    payload.get("code", "-"),
                    payload.get("message", "-"),
                    payload.get("ref", "-"),
                ),
            )
        else:
            log("SYS", f"tipo desconocido: {msg_type}")
            await session.send(
                self.envelope(
                    "error",
                    {
                        "code": "unknown_type",
                        "message": f"tipo {msg_type} desconocido",
                        "ref": message.get("id"),
                        "fatal": False,
                    },
                )
            )

    # -- consola ------------------------------------------------------------

    def help_text(self) -> str:
        return (
            "Comandos:\n"
            "  info                     datos del dispositivo conectado\n"
            "  state                    pide state (request what=state)\n"
            "  diag                     pide state + diagnostics\n"
            "  ping                     envia ping JSON\n"
            "  config [version]         envia config (sube la version)\n"
            "  on <canal> [segundos]    comando on (default 600 s)\n"
            "  off <canal>              comando off\n"
            "  auto cama_caliente       vuelve al termostato\n"
            "  open | close             comando de ventana\n"
            "  silence [segundos]       silencia la alarma (default 600 s)\n"
            "  reset                    limpia latches de alarma\n"
            "  raw <json>               envia un mensaje JSON tal cual\n"
            "  quit                     cierra el backend\n"
            "\nCanales: " + ", ".join(CHANNELS)
        )

    async def handle_console(self, line: str) -> None:
        line = line.strip()
        if not line:
            return
        if line == "help" or line == "?":
            print(self.help_text(), flush=True)
            return
        if line in ("quit", "exit"):
            if self.stop_event is not None:
                self.stop_event.set()
            return

        try:
            parts = shlex.split(line)
        except ValueError as exc:
            log("SYS", f"comando invalido: {exc}")
            return
        command = parts[0].lower()
        args = parts[1:]

        if command == "raw":
            if self.device is None:
                log("SYS", "no hay dispositivo conectado")
                return
            raw = line[len("raw"):].strip()
            await self.send_raw(self.device, raw)
            return

        if command == "info":
            if self.device is None:
                log("SYS", "no hay dispositivo conectado")
                return
            hello = self.device.hello
            log(
                "SYS",
                "device=%s boot=%s session=%s connected=%0.1fs fw=%s mode=%s "
                "cfg=%s local=%s source=%s"
                % (
                    self.device.device_id,
                    self.device.boot_id or "-",
                    self.device.session_id,
                    self.device.elapsed_s(),
                    hello.get("fw_version", "-"),
                    hello.get("mode", "-"),
                    hello.get("config_version", "-"),
                    hello.get("local_version", "-"),
                    hello.get("config_source", "-"),
                ),
            )
            return

        if self.device is None:
            log("SYS", "no hay dispositivo conectado (esperando hello)")
            return

        session = self.device

        try:
            if command == "state":
                await self.send_request(session, "state")
            elif command == "diag":
                await self.send_request(session, "diagnostics")
            elif command == "ping":
                await self.send_ping(session)
            elif command == "config":
                if args:
                    try:
                        self.config_version = max(1, int(args[0]))
                    except ValueError:
                        log("SYS", "config: version invalida")
                        return
                    await self.send_config(session, bump=False)
                else:
                    await self.send_config(session, bump=True)
            elif command == "on":
                if not args:
                    log("SYS", "uso: on <canal> [segundos]")
                    return
                channel = args[0]
                if channel not in CHANNELS:
                    log("SYS", f"canal desconocido: {channel}")
                    return
                duration = 600
                if len(args) >= 2:
                    try:
                        duration = int(args[1])
                    except ValueError:
                        log("SYS", "duracion invalida")
                        return
                await self.send_command(session, channel, "on", duration)
            elif command == "off":
                if not args:
                    log("SYS", "uso: off <canal>")
                    return
                channel = args[0]
                if channel not in CHANNELS:
                    log("SYS", f"canal desconocido: {channel}")
                    return
                await self.send_command(session, channel, "off")
            elif command == "auto":
                channel = args[0] if args else "cama_caliente"
                await self.send_command(session, channel, "auto")
            elif command == "open":
                await self.send_command(session, "ventana", "open")
            elif command == "close":
                await self.send_command(session, "ventana", "close")
            elif command == "silence":
                duration = 600
                if args:
                    try:
                        duration = int(args[0])
                    except ValueError:
                        log("SYS", "duracion invalida")
                        return
                await self.send_command(session, "alarma", "silence", duration)
            elif command == "reset":
                await self.send_command(session, "alarma", "reset")
            else:
                log("SYS", f"comando desconocido: {command} (usar help)")
        except ConnectionClosed:
            log("SYS", "el dispositivo se desconecto durante el comando")
        except Exception as exc:  # noqa: BLE001 - consola de desarrollo
            log("SYS", f"error enviando comando: {exc}")


# ---------------------------------------------------------------------------
# Servidor HTTP de claim
# ---------------------------------------------------------------------------


class ClaimHandler(BaseHTTPRequestHandler):
    backend: FakeBackend
    protocol_version = "HTTP/1.1"
    server_version = "ViveroFakeBackend/1.0"

    def _json(self, status: int, body: dict[str, Any]) -> None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:  # noqa: N802 - API de BaseHTTPRequestHandler
        if self.path.rstrip("/") == "/health":
            self._json(
                200,
                {
                    "ok": True,
                    "device_connected": self.backend.device is not None,
                    "claim_code": self.backend.claim_code,
                },
            )
        else:
            self._json(404, {"error": "not_found"})

    def do_POST(self) -> None:  # noqa: N802 - API de BaseHTTPRequestHandler
        if self.path.rstrip("/") != CLAIM_PATH:
            self._json(404, {"error": "not_found"})
            return

        try:
            length = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            length = 0
        raw = self.rfile.read(length) if length > 0 else b""
        try:
            request = json.loads(raw.decode("utf-8") or "{}")
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._json(400, {"error": "invalid_json"})
            return
        if not isinstance(request, dict):
            self._json(400, {"error": "invalid_json"})
            return

        device_id = str(request.get("device_id") or "").strip()
        claim_code = str(request.get("claim_code") or "").strip()
        log(
            "HTTP>",
            "claim device_id=%s claim_code=%s fw=%s hw=%s"
            % (
                device_id or "-",
                claim_code or "-",
                request.get("fw_version", "-"),
                request.get("hw_revision", "-"),
            ),
        )

        if self.backend.claim_code and claim_code != self.backend.claim_code:
            log("HTTP<", "403 invalid_claim")
            self._json(
                403,
                {
                    "error": "invalid_claim",
                    "message": "claim_code invalido",
                },
            )
            return

        if not device_id:
            device_id = f"vivero-{secrets.token_hex(3).upper()}"
        ws_url = self.backend.ws_url_for(self.headers.get("Host", ""))
        response = {
            "token": self.backend.token,
            "device_id": device_id,
            "ws_url": ws_url,
            "config_version": self.backend.config_version,
        }
        log(
            "HTTP<",
            "200 device_id=%s ws_url=%s cfg=%s"
            % (device_id, ws_url, self.backend.config_version),
        )
        self._json(200, response)

    def log_message(self, fmt: str, *args: Any) -> None:
        # Suprimimos el log por defecto; usamos log() con otro formato.
        return


def start_http_server(
    backend: FakeBackend, host: str, port: int
) -> tuple[ThreadingHTTPServer, threading.Thread]:
    handler = type(
        "BoundClaimHandler",
        (ClaimHandler,),
        {"backend": backend},
    )
    httpd = ThreadingHTTPServer((host, port), handler)
    httpd.daemon_threads = True
    thread = threading.Thread(
        target=httpd.serve_forever, name="fake-backend-http", daemon=True
    )
    thread.start()
    return httpd, thread


# ---------------------------------------------------------------------------
# Servidor WebSocket
# ---------------------------------------------------------------------------


def _header(ws: ServerConnection, name: str) -> str:
    request = getattr(ws, "request", None)
    if request is None:
        return ""
    value = request.headers.get(name)
    return str(value).strip() if value is not None else ""


async def heartbeat_loop(backend: FakeBackend, session: DeviceSession) -> None:
    interval = max(5, int(backend.args.heartbeat))
    try:
        while True:
            await asyncio.sleep(interval)
            if backend.device is not session:
                return
            await backend.send_ping(session)
    except asyncio.CancelledError:
        raise
    except ConnectionClosed:
        return
    except Exception as exc:  # noqa: BLE001 - no tumbar el servidor
        log("SYS", f"heartbeat: {exc}")


async def ws_handler(backend: FakeBackend, ws: ServerConnection) -> None:
    request = getattr(ws, "request", None)
    path = getattr(request, "path", "") if request is not None else ""
    path = path.split("?", 1)[0]
    if path.rstrip("/") != WS_PATH:
        log("WS", f"rechazo de path {path!r}")
        await ws.close(4404, "path invalido")
        return

    device_id = _header(ws, "X-Device-Id")
    auth = _header(ws, "Authorization")
    token = auth[7:].strip() if auth.lower().startswith("bearer ") else ""

    if not device_id or token != backend.token:
        log(
            "WS",
            "rechazo de %s: device_id=%s token=%s"
            % (
                getattr(ws, "remote_address", "?"),
                device_id or "-",
                "ok" if token else "-",
            ),
        )
        await ws.close(4401, "no autorizado")
        return

    previous = backend.device
    if previous is not None and previous.ws is not ws:
        log("WS", f"nueva sesion de {device_id}: cerrando la anterior con 4409")
        try:
            await previous.ws.close(4409, "otra sesion activa")
        except Exception:  # noqa: BLE001
            pass

    session = DeviceSession(ws, device_id)
    backend.device = session
    log(
        "WS",
        "conectado %s device_id=%s session=%s"
        % (getattr(ws, "remote_address", "?"), device_id, session.session_id),
    )

    heartbeat_task = asyncio.create_task(heartbeat_loop(backend, session))
    try:
        async for raw in ws:
            if isinstance(raw, bytes):
                log("SYS", "frame binario rechazado")
                await ws.close(4400, "solo texto")
                break
            await backend.on_device_message(session, raw)
    except ConnectionClosed:
        pass
    finally:
        heartbeat_task.cancel()
        try:
            await heartbeat_task
        except (asyncio.CancelledError, Exception):  # noqa: BLE001
            pass
        if backend.device is session:
            backend.device = None
        log(
            "WS",
            "desconectado %s session=%s duracion=%0.1fs"
            % (device_id, session.session_id, session.elapsed_s()),
        )


# ---------------------------------------------------------------------------
# Consola
# ---------------------------------------------------------------------------


def start_console(backend: FakeBackend, loop: asyncio.AbstractEventLoop) -> None:
    def worker() -> None:
        while True:
            try:
                line = input()
            except (EOFError, KeyboardInterrupt):
                return
            if not line.strip():
                continue
            loop.call_soon_threadsafe(
                lambda text=line: asyncio.create_task(backend.handle_console(text))
            )

    threading.Thread(target=worker, name="fake-backend-console", daemon=True).start()
    log("SYS", "consola interactiva lista (escribir 'help')")


# ---------------------------------------------------------------------------
# Modo normal
# ---------------------------------------------------------------------------


def print_banner(backend: FakeBackend, http_port: int, ws_port: int) -> None:
    addresses = local_ipv4_addresses() or ["127.0.0.1"]
    print()
    print("=" * 70)
    print(" Vivero IoT - backend de prueba")
    print("=" * 70)
    for ip in addresses:
        print(f" Claim HTTP : http://{ip}:{http_port}{CLAIM_PATH}")
        print(f" WebSocket  : ws://{ip}:{ws_port}{WS_PATH}")
        print()
    print(f" claim_code : {backend.claim_code or '(sin validacion)'}")
    print(f" token      : {backend.token}")
    print(f" config v   : {backend.config_version}")
    print(f" heartbeat  : {backend.args.heartbeat}s   telemetry: {backend.args.telemetry}s")
    print("=" * 70)
    print(" En el portal del dispositivo cargar la URL HTTP y el claim_code.")
    print(" Ejemplo de backend_url: " + f"http://{addresses[0]}:{http_port}")
    print("=" * 70)
    print()


async def run_normal(backend: FakeBackend) -> None:
    backend.stop_event = asyncio.Event()
    httpd = None
    try:
        httpd, _ = start_http_server(
            backend, backend.args.http_host, backend.args.http_port
        )
        http_port = int(httpd.server_address[1])

        async with serve(
            functools.partial(ws_handler, backend),
            backend.args.ws_host,
            backend.args.ws_port,
            max_size=MAX_FRAME_BYTES,
            compression=None,
            ping_interval=None,
        ) as server:
            backend.ws_port = int(server.sockets[0].getsockname()[1])
            print_banner(backend, http_port, backend.ws_port)
            if not backend.args.no_console and sys.stdin.isatty():
                start_console(backend, asyncio.get_running_loop())
            else:
                log("SYS", "consola deshabilitada; Ctrl+C para salir")
            await backend.stop_event.wait()
    finally:
        if httpd is not None:
            httpd.shutdown()
            httpd.server_close()
        backend.close()


# ---------------------------------------------------------------------------
# Autotest (sin hardware)
# ---------------------------------------------------------------------------


def http_post_json(url: str, payload: dict[str, Any]) -> tuple[int, dict[str, Any]]:
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return int(response.status), json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            body = json.loads(raw.decode("utf-8")) if raw else {}
        except (UnicodeDecodeError, json.JSONDecodeError):
            body = {}
        return int(exc.code), body


def envelope_for_test(
    msg_type: str, payload: dict[str, Any], msg_id: str
) -> dict[str, Any]:
    return {
        "v": PROTOCOL_VERSION,
        "type": msg_type,
        "id": msg_id,
        "ts": iso_now(),
        "payload": payload,
    }


async def recv_json(ws: Any, timeout: float = 3.0) -> dict[str, Any]:
    raw = await asyncio.wait_for(ws.recv(), timeout)
    if isinstance(raw, bytes):
        raise AssertionError("el servidor envio un frame binario")
    message = json.loads(raw)
    assert isinstance(message, dict), message
    return message


async def recv_type(ws: Any, expected: str, timeout: float = 5.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(f"timeout esperando {expected}")
        message = await recv_json(ws, remaining)
        if message.get("type") == expected:
            return message


async def selftest_device(backend: FakeBackend, http_port: int) -> None:
    base = f"http://127.0.0.1:{http_port}"

    # 1) claim_code incorrecto -> 403
    status, _ = http_post_json(
        base + CLAIM_PATH,
        {
            "device_id": "selftest-01",
            "claim_code": "CODIGO-INCORRECTO",
            "fw_version": "selftest",
            "hw_revision": "rev-A",
        },
    )
    assert status == 403, f"claim invalido devolvio {status}"
    log("TEST", "claim_code incorrecto -> 403 OK")

    # 2) claim valido -> token + ws_url
    status, claim = http_post_json(
        base + CLAIM_PATH,
        {
            "device_id": "selftest-01",
            "claim_code": backend.claim_code,
            "fw_version": "selftest",
            "hw_revision": "rev-A",
        },
    )
    assert status == 200, f"claim valido devolvio {status}"
    assert claim.get("token") == backend.token, claim
    assert claim.get("ws_url", "").startswith("ws://"), claim
    log("TEST", f"claim valido -> token y ws_url={claim['ws_url']} OK")

    # 3) token incorrecto -> cierre 4401
    ws_url = claim["ws_url"]
    try:
        async with connect(
            ws_url,
            additional_headers={
                "X-Device-Id": "selftest-01",
                "Authorization": "Bearer token-incorrecto",
            },
            max_size=MAX_FRAME_BYTES,
            ping_interval=None,
        ) as ws:
            try:
                await asyncio.wait_for(ws.recv(), 2)
            except ConnectionClosed:
                pass
            assert ws.close_code == 4401, f"cierre {ws.close_code}, se esperaba 4401"
        log("TEST", "token incorrecto -> cierre 4401 OK")
    except AssertionError:
        raise

    # 4) sesion valida: hello -> welcome -> config
    headers = {
        "X-Device-Id": "selftest-01",
        "Authorization": f"Bearer {backend.token}",
    }
    async with connect(
        ws_url,
        additional_headers=headers,
        max_size=MAX_FRAME_BYTES,
        ping_interval=None,
    ) as ws:
        hello = envelope_for_test(
            "hello",
            {
                "device_id": "selftest-01",
                "fw_version": "selftest",
                "hw_revision": "rev-A",
                "protocol": 1,
                "boot_id": "TESTBOOT",
                "uptime_s": 1,
                "config_version": 0,
                "local_version": 0,
                "config_source": "backend",
                "mode": "NORMAL",
                "time_valid": True,
                "ip": "127.0.0.1",
                "rssi_dbm": -40,
                "capabilities": {
                    "channels": list(CHANNELS),
                    "enabled_channels": list(CHANNELS),
                    "window_actuator": "pulse_toggle_5s",
                    "window_limits": 1,
                    "window_stop": False,
                },
            },
            "hello-test",
        )
        await ws.send(json.dumps(hello))
        welcome = await recv_type(ws, "welcome")
        assert welcome["payload"]["session_id"].startswith("s-"), welcome
        assert welcome["payload"]["heartbeat_s"] >= 5, welcome
        assert welcome["payload"]["send_config"] is True, welcome
        log("TEST", "hello -> welcome OK")

        config = await recv_type(ws, "config")
        assert config["id"].startswith("cfg-"), config
        assert config["payload"]["config_version"] == welcome["payload"]["config_version"], (
            config,
            welcome,
        )
        assert len(config["payload"]["channels"]) == 8, config
        log("TEST", "welcome -> config completo OK")

        await ws.send(
            json.dumps(
                envelope_for_test(
                    "ack",
                    {
                        "ref": config["id"],
                        "status": "ok",
                        "stored": True,
                        "applied": True,
                        "active_source": "backend",
                        "config_version": config["payload"]["config_version"],
                        "warnings": [],
                    },
                    "ack-cfg-test",
                )
            )
        )
        log("TEST", "ack de config OK")

        # 5) telemetria, estado y evento
        await ws.send(
            json.dumps(
                envelope_for_test(
                    "telemetry",
                    {
                        "boot_id": "TESTBOOT",
                        "seq": 1,
                        "buffered": False,
                        "samples": [
                            {
                                "ts": iso_now(),
                                "air": {"temp_c": 24.6, "rh_pct": 61.2, "valid": True},
                                "substrate": {
                                    "z1": {
                                        "moisture_pct": 38.1,
                                        "temp_c": 22.4,
                                        "valid": True,
                                    },
                                    "z2": {
                                        "moisture_pct": 41.7,
                                        "temp_c": 22.1,
                                        "valid": True,
                                    },
                                },
                                "bed": {"temp_c": 28.9, "valid": True},
                                "window": {
                                    "state": "closed",
                                    "limit_closed": True,
                                    "moving": False,
                                    "direction": None,
                                },
                                "rssi_dbm": -40,
                                "uptime_s": 30,
                                "time_valid": True,
                            }
                        ],
                    },
                    "tel-test",
                )
            )
        )
        await ws.send(
            json.dumps(
                envelope_for_test(
                    "state",
                    {
                        "boot_id": "TESTBOOT",
                        "mode": "NORMAL",
                        "config_version": config["payload"]["config_version"],
                        "local_version": 0,
                        "config_source": "backend",
                        "clock": {
                            "utc": iso_now(),
                            "source": "rtc",
                            "valid": True,
                            "timezone_offset_min": -180,
                        },
                        "wifi": {
                            "connected": True,
                            "rssi_dbm": -40,
                            "ip": "127.0.0.1",
                        },
                        "ws": {
                            "connected": True,
                            "session_id": welcome["payload"]["session_id"],
                            "last_rx_s": 1,
                        },
                        "channels": {
                            name: {
                                "enabled": True,
                                "on": False,
                                "source": "off",
                                "until": None,
                            }
                            for name in CHANNELS
                        },
                        "window": {
                            "state": "closed",
                            "moving": False,
                            "direction": None,
                            "fault": False,
                            "last_move_ts": None,
                        },
                        "alarms": {
                            "active": [],
                            "silenced_until": None,
                            "latched": [],
                        },
                    },
                    "state-test",
                )
            )
        )
        await ws.send(
            json.dumps(
                envelope_for_test(
                    "event",
                    {
                        "event_id": "TESTBOOT-0001",
                        "code": "BOOT",
                        "severity": "info",
                        "channel": None,
                        "message": "Arranque de prueba",
                        "data": {"reason": "selftest"},
                        "buffered": False,
                    },
                    "evt-test",
                )
            )
        )
        await asyncio.sleep(0.3)
        log("TEST", "telemetria, state y event enviados OK")

        # 6) comando del servidor -> ack del dispositivo
        command_id = await backend.send_command(
            backend.device, "riego_z1", "on", 60
        )
        command = await recv_type(ws, "command")
        assert command["id"] == command_id, command
        assert command["payload"]["channel"] == "riego_z1", command
        assert command["payload"]["action"] == "on", command
        await ws.send(
            json.dumps(
                envelope_for_test(
                    "ack",
                    {
                        "ref": command_id,
                        "status": "ok",
                        "channel": "riego_z1",
                        "applied": {"on": True, "until": iso_after(60)},
                        "warnings": [],
                    },
                    "ack-cmd-test",
                )
            )
        )
        log("TEST", f"command {command_id} -> dispositivo -> ack ok OK")

        # 7) ping -> pong
        await backend.send_ping(backend.device)
        ping = await recv_type(ws, "ping")
        assert ping["payload"].get("server_time"), ping
        await ws.send(
            json.dumps(
                envelope_for_test(
                    "pong",
                    {
                        "server_time_echo": ping["payload"]["server_time"],
                        "uptime_s": 60,
                    },
                    "pong-test",
                )
            )
        )
        log("TEST", "ping -> pong OK")

        # 8) request state -> state
        await backend.send_request(backend.device, "state")
        request = await recv_type(ws, "request")
        assert request["payload"].get("what") == "state", request
        await ws.send(
            json.dumps(
                envelope_for_test(
                    "state",
                    {
                        "boot_id": "TESTBOOT",
                        "mode": "NORMAL",
                        "config_version": config["payload"]["config_version"],
                        "local_version": 0,
                        "config_source": "backend",
                        "clock": {"valid": True, "timezone_offset_min": -180},
                        "wifi": {"connected": True, "rssi_dbm": -40},
                        "ws": {"connected": True},
                        "channels": {},
                        "window": {"state": "closed"},
                        "alarms": {"active": [], "latched": []},
                    },
                    "state-request-test",
                )
            )
        )
        log("TEST", "request state -> state OK")

        await asyncio.sleep(0.2)

    assert backend.stats["hello"] == 1, backend.stats
    assert backend.stats["telemetry"] == 1, backend.stats
    assert backend.stats["state"] >= 1, backend.stats
    assert backend.stats["event"] == 1, backend.stats
    assert backend.stats["ack"] >= 1, backend.stats
    assert backend.stats["pong"] == 1, backend.stats


async def run_selftest(backend: FakeBackend) -> None:
    backend.stop_event = asyncio.Event()
    httpd, _ = start_http_server(backend, "127.0.0.1", 0)
    http_port = int(httpd.server_address[1])
    try:
        async with serve(
            functools.partial(ws_handler, backend),
            "127.0.0.1",
            0,
            max_size=MAX_FRAME_BYTES,
            compression=None,
            ping_interval=None,
        ) as server:
            backend.ws_port = int(server.sockets[0].getsockname()[1])
            backend.args.public_ws_host = "127.0.0.1"
            log(
                "TEST",
                f"servidores en HTTP={http_port} WS={backend.ws_port}",
            )
            await selftest_device(backend, http_port)
            log("TEST", "SELFTEST OK")
    finally:
        httpd.shutdown()
        httpd.server_close()
        backend.close()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Backend de prueba para Vivero IoT (claim HTTP + WebSocket).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--http-host", default="0.0.0.0", help="bind HTTP")
    parser.add_argument("--http-port", type=int, default=8080, help="puerto HTTP")
    parser.add_argument("--ws-host", default="0.0.0.0", help="bind WebSocket")
    parser.add_argument("--ws-port", type=int, default=8081, help="puerto WebSocket")
    parser.add_argument(
        "--claim-code",
        default="VIVERO-01",
        help="claim_code aceptado; vacio desactiva la validacion",
    )
    parser.add_argument(
        "--token", default="dev-token", help="token que devuelve el claim"
    )
    parser.add_argument(
        "--config-version",
        type=int,
        default=100,
        help="version base de config",
    )
    parser.add_argument(
        "--heartbeat",
        type=int,
        default=25,
        help="segundos entre ping del servidor",
    )
    parser.add_argument(
        "--telemetry",
        type=int,
        default=30,
        help="telemetry_s sugerido en welcome",
    )
    parser.add_argument(
        "--expire-s",
        type=int,
        default=60,
        help="vigencia de los comandos enviados desde la consola",
    )
    parser.add_argument(
        "--no-auto-config",
        action="store_true",
        help="no enviar config automaticamente tras welcome",
    )
    parser.add_argument(
        "--no-console",
        action="store_true",
        help="no leer comandos de stdin",
    )
    parser.add_argument(
        "--csv",
        default=None,
        help="archivo CSV donde guardar la telemetria",
    )
    parser.add_argument(
        "--public-ws-host",
        default=None,
        help="host que se devuelve en ws_url (default: Host del claim)",
    )
    parser.add_argument(
        "--public-ws-url",
        default=None,
        help="ws_url completo que se devuelve en el claim",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="log detallado"
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="ejecuta una prueba automatica y termina",
    )
    args = parser.parse_args(argv)
    args.auto_config = not args.no_auto_config
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    backend = FakeBackend(args)
    try:
        if args.selftest:
            asyncio.run(run_selftest(backend))
            return 0
        asyncio.run(run_normal(backend))
        return 0
    except KeyboardInterrupt:
        print()
        log("SYS", "cerrando")
        return 0
    except OSError as exc:
        log("SYS", f"error de red: {exc}")
        return 1
    except Exception:  # noqa: BLE001 - CLI de desarrollo
        traceback.print_exc()
        return 1
    finally:
        backend.close()


if __name__ == "__main__":
    raise SystemExit(main())
