# Vivero IoT — Protocolo WebSocket (v0.1, propuesta)

**Fecha:** 16/11/2026
**Estado:** propuesta de este proyecto para acordar con el equipo de backend.
**Relacionado:** `00_vision_y_alcance.md`, `01_hardware_pinout.md`.

## 1. Principios

1. El ESP32 es **cliente WebSocket**. El backend es el servidor.
2. Todo el intercambio es **JSON UTF-8**, un mensaje por frame de texto.
3. El dispositivo nunca expone la base de datos ni recibe SQL.
4. La **habilitación local de relés** (portal cautivo) es la única fuente de verdad
   de qué canales existen físicamente. El backend no puede habilitar ni deshabilitar
   canales; solo puede leer ese estado y automatizar sobre los habilitados.
5. Sin conexión, el dispositivo sigue operando con la última `config` aplicada.
   El backend no es necesario para regar, ventilar, calefaccionar ni abrir/cerrar
   la ventana.
6. Toda orden remota es **idempotente y con vencimiento**. Si llega tarde, se
   descarta y se informa.
7. El backend debe poder reconstruir el estado completo a partir de `hello`,
   `state`, `telemetry` y `event`, sin depender de mensajes previos.

## 2. Transporte y conexión

| Ítem | Propuesta |
|---|---|
| URL | `ws://<host>:<puerto>/ws/device` |
| Identificación | header `X-Device-Id: <device_id>` |
| Autenticación | header `Authorization: Bearer <token>` |
| Subprotocolo | ninguno en v0.1 |
| Codificación | texto UTF-8; prohibido binario |
| Frame máximo | 16 KiB (dispositivo y servidor) |
| TLS | **sí**: `https://` en el claim y `wss://` en el WS, ambos con `setInsecure()` (cifra sin validar identidad). Validación de certificado pendiente |

- El dispositivo inicia la conexión. Nunca acepta conexiones entrantes.
- Si la librería WebSocket no permite headers personalizados, se acepta como
  alternativa `?device_id=<id>&token=<token>` en la URL. Es menos seguro porque
  el token queda en logs; usarlo solo en desarrollo.
- El servidor debe validar `device_id` + `token` antes de aceptar el upgrade.
  Códigos de cierre propuestos: `4401` token inválido, `4403` dispositivo no
  autorizado, `4409` otra sesión activa (si se permite una sola).
- Una sola sesión por dispositivo. Si se abre otra, el servidor cierra la anterior
  con `4409` o rechaza la nueva, según decida el backend.

## 3. Envoltorio y convenciones

Todo mensaje usa este sobre:

```json
{
  "v": 1,
  "type": "telemetry",
  "id": "m-000123",
  "ts": "2026-11-16T15:04:05Z",
  "payload": {}
}
```

| Campo | Tipo | Obligatorio | Descripción |
|---|---|---|---|
| `v` | int | sí | Versión del protocolo. Hoy `1`. |
| `type` | string | sí | Tipo de mensaje (ver tablas). |
| `id` | string | no | Identificador de traza del emisor. |
| `ts` | string \| null | sí | UTC ISO 8601 `YYYY-MM-DDTHH:MM:SSZ`. `null` si el reloj no es válido. |
| `payload` | object | sí | Contenido específico del tipo. Puede ser `{}`. |

Convenciones:

- Fechas siempre en **UTC** y con `Z`. La hora local solo se usa para evaluar
  horarios (`HH:MM`) y se deriva con `timezone_offset_min`.
- Números: enteros para segundos, versiones, secuencias y días; decimales para
  temperaturas, humedad y porcentajes.
- Unidades en el nombre del campo: `_c` (grados Celsius), `_pct` (0–100),
  `_s` (segundos), `_min` (minutos), `_w` (vatios, futuro).
- Sensor inválido: el objeto del sensor va con `"valid": false` y los valores
  numéricos en `null`. Nunca se envía `NaN` ni `Infinity` (JSON no los admite).
- Campos desconocidos: se ignoran. El emisor no debe asumir que el receptor los
  interpreta.
- Booleanos de estado de relé: `true` = canal **activo** (carga energizada),
  independientemente de la polaridad low-level del módulo.
- IDs de canal y de regla son estables y en `snake_case`. No se renombran entre
  versiones; si cambia el hardware, se agrega un canal nuevo.

## 4. Canales y métricas

### 4.1 Canales

| ID | Nombre | Tipo de actuador |
|---|---|---|
| `riego_z1` | Riego zona 1 (sustrato) | on/off con duración |
| `riego_z2` | Riego zona 2 (sustrato) | on/off con duración |
| `aspersion` | Riego por aspersión | on/off con duración |
| `cama_caliente` | Cama caliente | termostato on/off |
| `iluminacion` | Iluminación | on/off con duración |
| `ventilacion` | Ventilación forzada | on/off con duración |
| `ventana` | Ventana | abrir / cerrar / detener |
| `alarma` | Alarma | salida del motor de alarmas |

Los primeros siete aparecen en el menú manual local si están habilitados.
`alarma` no es comandable como un relé común: se controla con `silence`/`reset`.

### 4.2 Métricas de telemetría

| Métrica | Unidad | Fuente | Notas |
|---|---|---|---|
| `air_temp_c` | °C | DHT21 | `valid:false` si falla la lectura |
| `air_rh_pct` | % | DHT21 | 0–100 |
| `substrate1_moisture_pct` | % | ADC GPIO34 | calibración local dry/wet |
| `substrate2_moisture_pct` | % | ADC GPIO35 | calibración local |
| `substrate1_temp_c` | °C | DS18B20 GPIO16 | pin dedicado |
| `substrate2_temp_c` | °C | DS18B20 GPIO32 | pin dedicado |
| `bed_temp_c` | °C | DS18B20 | cama caliente |
| `window_state` | enum | sensor de cerrada | `closed`, `open`, `moving`, `fault`, `unknown` |
| `rssi_dbm` | dBm | WiFi | entero |
| `uptime_s` | s | firmware | entero |

Métricas derivadas que usa el motor de alarmas (no se transmiten como sensor):

- `sensor_fault_count`: cantidad de sensores obligatorios inválidos.
- `offline_s`: segundos desde el último contacto con el backend.
- `window_timeout`: `1` si el último movimiento no llegó al límite.
- `irrigation_timeout`: `1` si un riego se cortó por tiempo máximo.
- `clock_invalid`: `1` si el RTC no tiene hora válida.

## 5. Mensajes dispositivo → servidor

### 5.1 `hello`

Primer mensaje tras cada conexión. Permite al backend identificar el equipo y
decidir si debe enviar configuración.

```json
{
  "v": 1,
  "type": "hello",
  "id": "hello-a1b2c3d4",
  "ts": "2026-11-16T15:04:05Z",
  "payload": {
    "device_id": "vivero-01",
    "fw_version": "0.1.0",
    "hw_revision": "rev-A",
    "protocol": 1,
    "boot_id": "a1b2c3d4",
    "uptime_s": 12,
    "config_version": 7,
    "local_version": 3,
    "config_source": "backend",
    "mode": "NORMAL",
    "time_valid": true,
    "ip": "192.168.1.50",
    "rssi_dbm": -61,
    "capabilities": {
      "channels": ["riego_z1", "riego_z2", "aspersion", "cama_caliente",
                   "iluminacion", "ventilacion", "ventana", "alarma"],
      "enabled_channels": ["riego_z1", "riego_z2", "cama_caliente",
                           "iluminacion", "ventilacion", "ventana", "alarma"],
      "window_actuator": "pulse_toggle_5s",
      "window_limits": 1,
      "window_stop": false
    }
  }
}
```

- `enabled_channels` es el estado **local** del portal. El backend lo guarda para
  mostrar el equipo, pero no lo modifica.
- `window_actuator`: `pulse_toggle_5s` (un relé, pulso de 5 s, el controlador
  alterna el sentido), `reversible` (requiere segundo relé/DPDT) o `none`.
- `window_limits`: `0` o `1` en v1; `2` si en v2 se agrega el sensor de abierta.
- `window_stop`: `false` en v1 porque el controlador toggle no tiene entrada de
  parada. `true` solo si el hardware futuro la incorpora.
- `config_source`: `backend` si la config activa vino del servidor, `local` si el
  operador activó el perfil local. `config_version` es la última versión de backend
  almacenada; `local_version` es la del perfil local.

### 5.2 `telemetry`

Muestras de sensores. Se envía una muestra cada 30 s por defecto (ajustable con
`welcome.payload.telemetry_s`) y en lotes de hasta 20 al recuperar una cola
offline.

```json
{
  "v": 1,
  "type": "telemetry",
  "ts": "2026-11-16T15:04:30Z",
  "payload": {
    "boot_id": "a1b2c3d4",
    "seq": 1042,
    "buffered": false,
    "samples": [
      {
        "ts": "2026-11-16T15:04:30Z",
        "air": { "temp_c": 24.6, "rh_pct": 61.2, "valid": true },
        "substrate": {
          "z1": { "moisture_pct": 38.1, "temp_c": 22.4, "valid": true },
          "z2": { "moisture_pct": 41.7, "temp_c": 22.1, "valid": true }
        },
        "bed": { "temp_c": 28.9, "valid": true },
        "window": {
          "state": "closed",
          "limit_closed": true,
          "moving": false,
          "direction": null
        },
        "rssi_dbm": -61,
        "uptime_s": 1470,
        "time_valid": true
      }
    ]
  }
}
```

- `seq` es monótono por arranque y por muestra (no por lote). El servidor deduplica
  por `(device_id, boot_id, seq)`.
- `buffered: true` indica que la muestra se tomó sin conexión y se envía tarde.
  El `ts` de cada muestra es el de captura, no el de envío.
- En un lote, `payload.ts` es la hora de envío; cada muestra conserva su `ts`.
- `window.state`: `closed` si el sensor de cerrada está activo; `open` si está
  inactivo y no hay movimiento; `moving` durante el pulso/viaje; `fault` si el
  sensor no cambió tras el pulso. En v1 no hay `limit_open`.

### 5.3 `state`

Foto completa del estado lógico y de los actuadores. Se envía:

- inmediatamente después de `hello`;
- ante cualquier cambio de salida, modo, alarma o ventana;
- cada 5 minutos como refresco;
- en respuesta a `request` con `what: "state"`.

```json
{
  "v": 1,
  "type": "state",
  "ts": "2026-11-16T15:05:00Z",
  "payload": {
    "boot_id": "a1b2c3d4",
    "mode": "NORMAL",
    "config_version": 7,
    "local_version": 3,
    "config_source": "backend",
    "clock": {
      "utc": "2026-11-16T15:05:00Z",
      "source": "rtc",
      "valid": true,
      "timezone_offset_min": -180
    },
    "wifi": { "connected": true, "rssi_dbm": -61, "ip": "192.168.1.50" },
    "ws": { "connected": true, "session_id": "s-8891", "last_rx_s": 2 },
    "channels": {
      "riego_z1": { "enabled": true, "on": false, "source": "off", "until": null },
      "riego_z2": { "enabled": true, "on": false, "source": "off", "until": null },
      "aspersion": { "enabled": false, "on": false, "source": "off", "until": null },
      "cama_caliente": { "enabled": true, "on": true, "source": "auto", "until": null },
      "iluminacion": { "enabled": true, "on": false, "source": "off", "until": null },
      "ventilacion": { "enabled": true, "on": false, "source": "off", "until": null },
      "ventana": { "enabled": true, "on": false, "source": "off", "until": null },
      "alarma": { "enabled": true, "on": false, "source": "off", "until": null }
    },
    "window": {
      "state": "closed",
      "moving": false,
      "direction": null,
      "fault": false,
      "last_move_ts": null
    },
    "alarms": {
      "active": [],
      "silenced_until": null,
      "latched": []
    }
  }
}
```

- `source`: `off`, `auto`, `manual_local`, `manual_remote`, `alarm`, `test`.
- `until`: fin previsto de la orden manual o de la automatización, en UTC; `null`
  si no aplica (termostato, alarma, ventana).
- `enabled` refleja el portal local. El backend no lo cambia.
- `config_source`: `backend` o `local`; indica qué perfil está gobernando.
  `config_version` es la última versión de backend almacenada (puede no estar
  activa si hay override local); `local_version` es el perfil local.
- `window.direction`: `opening`, `closing` o `null`.
- No se envía `state` en cada telemetría: los cambios se detectan por diferencia
  contra el último `state` recibido.

### 5.4 `event`

Hecho relevante para auditoría o alerta. Se envía apenas ocurre; si no hay
conexión, se encola.

```json
{
  "v": 1,
  "type": "event",
  "ts": "2026-11-16T15:06:12Z",
  "payload": {
    "event_id": "a1b2c3d4-0007",
    "code": "IRRIGATION_START",
    "severity": "info",
    "channel": "riego_z1",
    "message": "Riego Z1 iniciado por horario",
    "data": { "reason": "schedule", "duration_s": 600, "entry_id": "z1-1" },
    "buffered": false
  }
}
```

Códigos y severidad propuestos:

| Código | Severidad | Cuándo |
|---|---|---|
| `BOOT` | info | arranque; `data.reason`: `power_on`, `watchdog`, `reset` |
| `WIFI_CONNECTED` / `WIFI_LOST` | info / warning | enlace WiFi |
| `WS_CONNECTED` / `WS_LOST` | info / warning | sesión WebSocket |
| `PORTAL_ENTER` / `PORTAL_EXIT` | info | portal cautivo |
| `CONFIG_APPLIED` | info | configuración validada y persistida |
| `CONFIG_REJECTED` | error | configuración inválida; `data.errors` |
| `CONFIG_SOURCE_CHANGED` | info | `data.source`: `backend` o `local` |
| `TIME_SYNCED` | info | NTP/servidor corrigió el reloj |
| `TIME_INVALID` | warning | RTC sin hora válida |
| `SENSOR_FAULT` / `SENSOR_RECOVERED` | warning / info | `data.sensor` |
| `DISPLAY_FAULT` | warning | OLED/I2C no responde; el control sigue |
| `IRRIGATION_START` | info | `data.reason`: `schedule`, `threshold`, `manual_local`, `manual_remote` |
| `IRRIGATION_STOP` | info | `data.reason`: `done`, `remote`, `manual_local`, `max_run`, `interlock`, `sensor` |
| `IRRIGATION_SKIPPED` | warning | `data.reason`: `interlock`, `overlap`, `min_off`, `invalid_sensor` |
| `SCHEDULE_MISSED` | info | arranque fuera de la ventana de gracia; `data.entry_id` |
| `THERMOSTAT_ON` / `THERMOSTAT_OFF` | info | cama caliente |
| `LIGHT_ON` / `LIGHT_OFF` | info | iluminación |
| `FAN_ON` / `FAN_OFF` | info | ventilación |
| `WINDOW_OPEN` / `WINDOW_CLOSE` | info | orden de movimiento aceptada |
| `WINDOW_STOP` | info | movimiento detenido (solo hardware con parada) |
| `WINDOW_TIMEOUT` | warning | no llegó al fin de carrera o no cambió el sensor |
| `WINDOW_FAULT` | warning | sensor trabado o estado inconsistente |
| `ALARM_RAISED` | warning/critical | `data.rule_id`, `data.value` |
| `ALARM_CLEARED` | info | condición normalizada |
| `ALARM_SILENCED` | info | `data.duration_s` |
| `COMMAND_REJECTED` | warning | `data.reason` |
| `WATCHDOG_RESET` | error | reinicio por watchdog |
| `NVS_ERROR` | error | fallo de persistencia |
| `LOW_MEMORY` | warning | heap bajo |
| `DIAGNOSTICS` | info | respuesta a `request what:"diagnostics"` |

- `event_id` único: `<boot_id>-<secuencia>`. El servidor deduplica por ese ID.
  Los eventos críticos restaurados desde NVS conservan el `boot_id` original, no
  el del arranque actual, para que la deduplicación siga siendo válida.
- `severity`: `info`, `warning`, `error`, `critical`.
- `data` es libre pero estable por código; no meter texto que ya está en `message`.

### 5.5 `ack`

Confirma `config`, `command` o `request`. `payload.ref` es el `id` del mensaje
confirmado.

```json
{
  "v": 1,
  "type": "ack",
  "ts": "2026-11-16T15:06:13Z",
  "payload": {
    "ref": "cmd-7f3a",
    "status": "ok",
    "channel": "riego_z1",
    "applied": { "on": true, "until": "2026-11-16T15:16:13Z" },
    "warnings": []
  }
}
```

Estados:

| `status` | Significado | `data`/campos |
|---|---|---|
| `ok` | aplicado | `applied` con el efecto real |
| `rejected` | no aplicado por regla | `reason`: `channel_disabled`, `invalid_payload`, `safety_interlock`, `busy`, `fault`, `clock_invalid`, `local_manual_active`, `rate_limited`, `unsupported` |
| `expired` | llegó después de `expires_at` | `reason: "expired"` |
| `error` | fallo interno | `reason`: `nvs`, `sensor`, `actuator` |

- Un `ack` siempre se emite, incluso para comandos rechazados o vencidos.
- Para `config`, `applied.config_version` indica la versión efectiva. Si el perfil
  local está activo, se devuelve `stored: true`, `applied: false` y
  `active_source: "local"`; la config queda cacheada para cuando se vuelva al
  backend.

### 5.6 `pong`

Respuesta a `ping`.

```json
{
  "v": 1,
  "type": "pong",
  "ts": "2026-11-16T15:07:00Z",
  "payload": { "server_time_echo": "2026-11-16T15:07:00Z", "uptime_s": 1620 }
}
```

## 6. Mensajes servidor → dispositivo

### 6.1 `welcome`

Respuesta a `hello`. Fija la sesión y el ritmo de heartbeat.

```json
{
  "v": 1,
  "type": "welcome",
  "ts": "2026-11-16T15:04:06Z",
  "payload": {
    "session_id": "s-8891",
    "server_time": "2026-11-16T15:04:06Z",
    "heartbeat_s": 25,
    "telemetry_s": 30,
    "config_version": 8,
    "send_config": true,
    "limits": { "max_frame_bytes": 16384, "max_commands_per_min": 30 }
  }
}
```

- Si `send_config` es `true` y `config_version` difiere de la del dispositivo, el
  servidor envía `config` a continuación.
- `heartbeat_s` recomendado: 25 s. El dispositivo responde cada `ping` y considera
  caída la sesión si pasan 2 intervalos sin recibir nada.
- `telemetry_s` recomendado: 30 s. El backend puede subirlo para ahorrar tráfico;
  el mínimo es 10 s. No afecta la frecuencia de muestreo local ni la de las
  alarmas.

### 6.2 `config`

Configuración completa y versionada. Reemplaza la anterior de forma atómica.

```json
{
  "v": 1,
  "type": "config",
  "id": "cfg-0008",
  "ts": "2026-11-16T15:04:07Z",
  "payload": {
    "config_version": 8,
    "timezone_offset_min": -180,
    "irrigation": {
      "interlock": true,
      "stagger_s": 30
    },
    "channels": {
      "riego_z1": {
        "auto_enabled": true,
        "schedule": {
          "enabled": true,
          "entries": [
            { "id": "z1-1", "start": "07:00", "duration_s": 600,
              "days": [1, 2, 3, 4, 5, 6, 7] }
          ]
        },
        "moisture": {
          "enabled": true,
          "start_below_pct": 30.0,
          "stop_above_pct": 45.0,
          "max_run_s": 900,
          "min_off_s": 1800,
          "window_start": "06:00",
          "window_end": "20:00"
        }
      },
      "riego_z2": {
        "auto_enabled": true,
        "schedule": {
          "enabled": true,
          "entries": [
            { "id": "z2-1", "start": "07:30", "duration_s": 600,
              "days": [1, 3, 5] }
          ]
        },
        "moisture": {
          "enabled": false,
          "start_below_pct": 30.0,
          "stop_above_pct": 45.0,
          "max_run_s": 900,
          "min_off_s": 1800,
          "window_start": "06:00",
          "window_end": "20:00"
        }
      },
      "aspersion": {
        "auto_enabled": true,
        "schedule": {
          "enabled": true,
          "entries": [
            { "id": "asp-1", "start": "18:00", "duration_s": 300,
              "days": [1, 2, 3, 4, 5, 6, 7] }
          ]
        },
        "max_run_s": 1800
      },
      "cama_caliente": {
        "auto_enabled": true,
        "thermostat": {
          "setpoint_c": 28.0,
          "hysteresis_c": 1.0,
          "max_c": 35.0,
          "sensor": "bed"
        }
      },
      "iluminacion": {
        "auto_enabled": true,
        "max_run_s": 57600,
        "schedule": {
          "enabled": true,
          "entries": [
            { "id": "luz-1", "start": "06:00", "duration_s": 43200,
              "days": [1, 2, 3, 4, 5, 6, 7] }
          ]
        }
      },
      "ventilacion": {
        "auto_enabled": true,
        "schedule": { "enabled": false, "entries": [] },
        "climate": {
          "enabled": true,
          "on_above_temp_c": 30.0,
          "off_below_temp_c": 27.0,
          "on_above_rh_pct": 85.0,
          "off_below_rh_pct": 75.0,
          "min_on_s": 60,
          "min_off_s": 120,
          "max_run_s": 3600
        }
      },
      "ventana": {
        "auto_enabled": true,
        "climate": {
          "open_above_temp_c": 32.0,
          "close_below_temp_c": 26.0,
          "open_above_rh_pct": 90.0,
          "close_below_rh_pct": 80.0
        },
        "pulse_ms": 5000,
        "travel_time_s": 20,
        "max_travel_s": 30,
        "min_interval_s": 60
      },
      "alarma": { "auto_enabled": true }
    },
    "alarms": {
      "enabled": true,
      "silence_max_s": 3600,
      "rules": [
        { "id": "air_temp_high", "enabled": true, "metric": "air_temp_c",
          "op": ">", "threshold": 35.0, "hysteresis": 1.0, "delay_s": 60,
          "severity": "critical", "latch": false },
        { "id": "air_temp_low", "enabled": true, "metric": "air_temp_c",
          "op": "<", "threshold": 5.0, "hysteresis": 1.0, "delay_s": 60,
          "severity": "warning", "latch": false },
        { "id": "bed_temp_high", "enabled": true, "metric": "bed_temp_c",
          "op": ">", "threshold": 34.0, "hysteresis": 1.0, "delay_s": 30,
          "severity": "critical", "latch": false },
        { "id": "sensor_fault", "enabled": true, "metric": "sensor_fault_count",
          "op": ">=", "threshold": 1, "hysteresis": 0, "delay_s": 120,
          "severity": "warning", "latch": false },
        { "id": "window_timeout", "enabled": true, "metric": "window_timeout",
          "op": "==", "threshold": 1, "hysteresis": 0, "delay_s": 0,
          "severity": "warning", "latch": true },
        { "id": "offline", "enabled": true, "metric": "offline_s",
          "op": ">", "threshold": 600, "hysteresis": 0, "delay_s": 0,
          "severity": "warning", "latch": false }
      ]
    }
  }
}
```

Reglas de configuración:

- `config_version` es entero creciente, definido por el backend. El dispositivo
  aplica solo si la fuente activa es `backend` y la versión es mayor que la
  almacenada, salvo `force: true`.
- `timezone_offset_min`: minutos respecto de UTC (UTC-3 = `-180`). Rango
  `-720..840`.
- `days`: 1 = lunes … 7 = domingo, en hora local.
- `start`/`window_start`/`window_end`: `HH:MM` en hora local, 24 h.
- `duration_s` de iluminación puede ser 43200 (12 h); el máximo por entrada es
  86400.
- `enabled` a nivel de canal es **local**. Si el backend lo envía, se ignora y se
  devuelve en `warnings`. Para apagar la automatización de un canal se usa
  `auto_enabled: false`, que no apaga el relé por sí solo: la salida pasa a OFF
  salvo que haya una orden manual activa.
- `channels` es parcialmente opcional: si falta un canal, conserva la config
  anterior de ese canal. Una `config` completa siempre incluye los ocho.
- Rangos: humedad `0..100`, temperaturas `-20..80`, `setpoint_c <= max_c`,
  duraciones `1..86400`, `stagger_s 0..600`, `hysteresis >= 0`,
  `pulse_ms 1000..10000`, `min_interval_s 10..300`.
- `ventana.travel_time_s` es el tiempo para declarar `OPEN` cuando el sensor de
  cerrada se libera; `max_travel_s` es el máximo que se espera la transición. Si
  `max_travel_s < pulse_ms/1000` o `travel_time_s > max_travel_s`, la config es
  inválida.
- `ventana.min_interval_s` protege el controlador: mínimo entre pulsos.
- Si la config local está activa (`config_source: "local"`), esta `config` se
  valida y se guarda, pero **no se aplica**. El `ack` devuelve `stored: true` y
  `applied: false`; el backend lo ve en `state.config_source`.
- Si cualquier campo es inválido, se rechaza la config **completa** y se conserva
  la anterior. No se aplican cambios parciales.
- El dispositivo persiste la config válida en NVS antes de confirmar `ok`.
- El servidor puede enviar `config` en cualquier momento; el dispositivo la aplica
  sin reiniciar y sin cortar la automatización en curso (la evalúa en el próximo
  ciclo, salvo que la config apague esa función, en cuyo caso la salida se apaga).

### 6.3 `command`

Orden puntual del backend. No reemplaza la configuración: es una acción con
vencimiento.

```json
{
  "v": 1,
  "type": "command",
  "id": "cmd-7f3a",
  "ts": "2026-11-16T15:06:10Z",
  "payload": {
    "channel": "riego_z1",
    "action": "on",
    "duration_s": 600,
    "expires_at": "2026-11-16T15:06:40Z",
    "requested_by": "web:user42",
    "reason": "manual"
  }
}
```

Acciones por canal:

| Canal | Acciones | Campos |
|---|---|---|
| `riego_z1`, `riego_z2`, `aspersion` | `on`, `off` | `duration_s` obligatorio en `on` (1..`max_run_s`) |
| `iluminacion`, `ventilacion` | `on`, `off` | `duration_s` opcional; default 600 s; máximo `max_run_s` |
| `cama_caliente` | `on`, `off`, `auto` | `on` fuerza encendido con corte por `max_c`; `duration_s` opcional, default 1800 s; `auto` vuelve al termostato |
| `ventana` | `open`, `close` | sin `duration_s`; pulso de `pulse_ms`; `stop` no soportado en v1 (`unsupported`) |
| `alarma` | `silence`, `reset` | `duration_s` en `silence` (1..`silence_max_s`); `reset` limpia latches |
| — | `report_state` | sin `channel`; el dispositivo responde `state` |

Reglas de aceptación:

1. `expires_at` es obligatorio y debe estar en UTC. Si el reloj del dispositivo no
   es válido, solo se aceptan acciones seguras (`off`, `close`, `silence`,
   `reset`); las demás se rechazan con `reason: "clock_invalid"`.
2. Comando vencido → `ack` con `status: "expired"`, sin tocar el actuador.
3. Canal no habilitado en el portal → `rejected`, `reason: "channel_disabled"`.
4. `on`/`open` remoto mientras hay una orden manual local activa → `rejected`,
   `reason: "local_manual_active"`. Las acciones seguras (`off`, `close`,
   `silence`, `reset`) sí se aceptan y tienen prioridad.
5. `duration_s` mayor que el máximo configurado se recorta al máximo y se informa
   en `warnings`, o se rechaza si el backend pide `strict: true`. Default:
   recortar.
6. Límite de ráfaga: 1 comando por canal cada 2 s y 30 comandos por minuto por
   dispositivo. Excedido → `rejected`, `reason: "rate_limited"`.
7. El `ack` refleja el estado **después** de aplicar, no la intención:
   `applied.on`, `applied.until`, `applied.source`.

Reglas específicas de la ventana (controlador toggle):

- `open` solo se acepta si el estado es `closed` (sensor de cerrada activo).
- `close` solo se acepta si el estado es `open` (sensor inactivo y sin movimiento).
- Si ya está en el estado pedido, se responde `ok` con `applied.noop: true`.
- En `moving` se rechaza con `busy`; en `fault` se rechaza con `fault`.
- El pulso dura `pulse_ms` (default 5000 ms). Después se espera la transición del
  sensor hasta `max_travel_s`; si no cambia, `WINDOW_FAULT` + alarma.
- Al abrir, el sensor de cerrada se libera al iniciar el movimiento; el estado
  `open` se declara tras `travel_time_s`. No hay confirmación de apertura total en
  v1; el controlador maneja su límite.
- `stop` no existe en v1: el controlador toggle no tiene entrada de parada. El
  firmware responde `rejected`, `reason: "unsupported"`.

### 6.4 `request`

Petición de diagnóstico del backend.

```json
{
  "v": 1,
  "type": "request",
  "id": "req-221",
  "ts": "2026-11-16T15:07:00Z",
  "payload": { "what": "state" }
}
```

`what`: `state` (responde `state`) o `diagnostics` (responde `state` + un `event`
`DIAGNOSTICS` con heap, tareas, uptime, reconexiones y versión). No se usa para
cambiar configuración ni para comandos.

### 6.5 `ping`

```json
{
  "v": 1,
  "type": "ping",
  "ts": "2026-11-16T15:07:00Z",
  "payload": { "server_time": "2026-11-16T15:07:00Z" }
}
```

El dispositivo responde `pong` en menos de 5 s. Si acumula 2 `ping` sin
respuesta, el servidor puede cerrar la sesión; el dispositivo también cierra si
pasan 2 intervalos sin recibir ningún frame.

### 6.6 `error`

```json
{
  "v": 1,
  "type": "error",
  "ts": "2026-11-16T15:07:01Z",
  "payload": {
    "code": "invalid_json",
    "message": "frame no parseable",
    "ref": null,
    "fatal": false
  }
}
```

Códigos: `invalid_json`, `unsupported_version`, `unknown_type`,
`invalid_payload`, `unauthorized`, `rate_limited`, `internal`.
`fatal: true` indica que el servidor cerrará la sesión.

## 7. Motor de alarmas

- El dispositivo evalúa las reglas localmente en cada ciclo (1 s). El backend
  recibe `ALARM_RAISED` / `ALARM_CLEARED`, pero **no** es necesario para que la
  alarma suene.
- Una regla se activa cuando la métrica cumple `op`/`threshold` durante `delay_s`.
  Se desactiva cuando vuelve al otro lado de `threshold ± hysteresis` durante el
  mismo `delay_s` (o de inmediato si `hysteresis = 0` y `delay_s = 0`).
- Varias reglas pueden estar activas a la vez: `alarms.active` es un array de
  `rule_id`. El relé de alarma se energiza si hay al menos una activa y no está
  silenciada.
- `latch: true`: la regla queda en `alarms.latched` aunque la condición se
  normalice; solo `command action:"reset"` la limpia. Al limpiar un latch cuya
  condición sigue presente, se reactiva.
- `silence` apaga el relé durante `duration_s` sin borrar las condiciones. Si
  aparece una alarma nueva de severidad `critical` durante el silencio, vuelve a
  energizar y se informa `ALARM_RAISED` con `data.resilenced: true`.
- La regla `offline` se evalúa con `offline_s` (tiempo desde el último frame del
  servidor). Suena localmente aunque no haya conexión; el evento se encola.
- Si una métrica es inválida, las reglas que dependen de ella no se evalúan; se
  activa `sensor_fault` si corresponde. Nunca se interpreta un `null` como 0.
- La alarma no se auto-silencia por reinicio: al arrancar se evalúan las
  condiciones y, si persisten, vuelve a sonar. Los latches se persisten en NVS.

## 8. Tiempo y zonas horarias

- El dispositivo tiene RTC DS3231. Si es válido, sobrevive cortes de energía.
- Fuentes, en orden de prioridad: `server_time` del backend > NTP > RTC.
- Al recibir `welcome` o `ping`, si el reloj está inválido o la diferencia supera
  5 s, se ajusta el RTC y se emite `TIME_SYNCED`.
- Si no hay hora válida: no se ejecutan horarios; el riego por umbral y el
  termostato sí funcionan; los comandos remotos se restringen según §6.3.
- Los horarios `HH:MM` se evalúan en hora local = UTC + `timezone_offset_min`.
  No hay cambio de horario automático en v0.1: el backend actualiza el offset dos
  veces al año si corresponde.
- Una entrada de horario se dispara una vez por día. Si el dispositivo arranca o
  sale de un corte dentro de los **60 s** posteriores a la hora de inicio, se
  ejecuta; pasado ese margen, se omite y se registra `SCHEDULE_MISSED` (evento
  nuevo, severidad info). No se riega retroactivamente para evitar sorpresas.
- La evaluación usa el minuto local y `entry_id` + fecha para deduplicar, incluso
  si el loop se ejecuta varias veces por segundo.

## 9. Reconexión, heartbeat y cola offline

### 9.1 Reconexión

- WiFi: si se cae, reintenta cada 10 s; a los 60 s sin enlace entra a `PORTAL`
  (ver `03_reglas_y_plan.md` §2). Al salir del portal reintenta STA sin reiniciar.
- WebSocket: backoff exponencial 2, 4, 8, 16, 32, 60 s, con jitter ±20 % y techo
  de 60 s. Se reinicia al alcanzar 60 s de sesión estable.
- No se reinicia el ESP32 por perder WebSocket. La automatización y el control
  local siguen. Un watchdog solo actúa si una tarea se cuelga.
- Tras reconectar: `hello` → (si corresponde) `config` → `state` → eventos
  pendientes → telemetría pendiente. El orden prioriza lo accionable.

### 9.2 Cola offline

| Cola | Capacidad | Persistencia | Política |
|---|---|---|---|
| Telemetría | 720 muestras (~6 h a 30 s) | RAM | al llenarse, se descarta la más antigua |
| Eventos | 64 | RAM | al llenarse, se descarta el más antiguo no crítico; si todos son críticos, el más antiguo (sigue en NVS) |
| Eventos críticos | 8 | NVS (ring) | `ALARM_RAISED` crítico, `CONFIG_REJECTED`, `WATCHDOG_RESET`, `NVS_ERROR` |
| Estado | 1 (el último) | — | no se encola; se envía al reconectar |

- La telemetría se envía en lotes de hasta 20 muestras, con `buffered: true`.
- Los eventos se envían de a uno, en orden cronológico.
- Los eventos críticos se escriben en un ring NVS antes de confirmarse y se
  liberan cuando el frame se envió por WebSocket. Si el equipo reinicia antes,
  se restauran al arrancar y se reenvían; el backend los deduplica por
  `event_id`. El ring conserva el `boot_id` y la hora original de cada evento.
- Si el ring de eventos críticos se llena, se descarta el más antiguo y se
  registra `NVS_ERROR` una sola vez por arranque. Un fallo de escritura NVS
  también genera `NVS_ERROR` (sin persistirlo, para no realimentar el error).
- El backend debe aceptar mensajes con `ts` antiguo y no descartarlos por llegar
  fuera de orden.

### 9.3 Detección de enlace caído

- `offline_s` se calcula desde el último frame válido recibido del servidor.
- A los 60 s sin frames se marca `WS_LOST` y se empieza a bufferizar.
- El relé de alarma por `offline` se evalúa localmente; no depende de que el
  backend esté disponible.

## 10. Idempotencia, orden y deduplicación

- **Telemetría:** clave `(device_id, boot_id, seq)`. El servidor ignora duplicados.
- **Eventos:** clave `event_id`. El servidor ignora duplicados; si el mismo hecho
  se reintenta, conserva el primero.
- **Comandos:** el dispositivo guarda los últimos 8 `id` procesados durante
  10 minutos. Si llega un duplicado, responde el mismo `ack` sin reaplicar.
- **Config:** se aplica solo si `config_source = backend` y `config_version` es
  mayor que la almacenada, o si `force: true`. Si hay override local, se valida y
  se guarda, pero no se aplica; siempre se confirma con `ack` indicando `stored` y
  `applied`.
- **Estado:** el último `state` de un `boot_id` reemplaza a los anteriores. Un
  `boot_id` nuevo invalida el estado previo del dispositivo.
- El servidor debe tolerar mensajes duplicados y fuera de orden; el dispositivo
  debe tolerar `config` repetida y comandos repetidos.

## 11. Provisioning y credenciales

### 11.1 Flujo recomendado (código de reclamo)

1. Fábrica: el dispositivo trae `device_id` único y un `claim_code` de un solo uso
   (etiqueta/QR). También puede generarse en el primer arranque y mostrarse en el
   OLED.
2. Portal cautivo: el operador carga WiFi, URL base del backend y `claim_code`.
   No carga el token a mano.
3. Primer `NORMAL`: el dispositivo hace `POST <http_base>/api/v1/devices/claim`
   con `{device_id, claim_code, fw_version, hw_revision}`.
4. El backend responde `{token, ws_url, config_version}`. El dispositivo guarda
   token y URL en NVS y abre el WebSocket.
5. Si el claim falla, reintenta con backoff (60 s si no hay red/HTTP; 5 min si
   el backend rechaza el código) y **sigue operando offline** con la última
   configuración válida. No se fuerza `PORTAL` para no interrumpir riego,
   ventilación ni clima. El error se muestra en el portal y en el OLED; el
   operador puede entrar manualmente al portal para corregir la URL o el
   `claim_code`. Sin identidad/token no se abre WebSocket, pero la automatización
   local y los botones siguen activos.

Ventajas: no se tipea un token largo en el portal; el token se puede rotar y
revocar por dispositivo. **Pendiente:** el equipo de backend debe confirmar este
endpoint o proponer otro.

### 11.2 Alternativas

- **Token preimpreso:** el portal pide `device_id` + `token`; el backend los tiene
  cargados de antemano. Más simple para el backend, más incómodo en campo.
- **Alta desde el backend:** el operador crea el dispositivo en la web y carga el
  `device_id`; el dispositivo solo necesita la URL. Requiere un canal de confianza
  para el primer token; no recomendado sin TLS.

### 11.3 Rotación y revocación

- Rotación: el backend puede enviar `payload.token` en `welcome`; el dispositivo
  lo persiste antes de confirmar. **Implementado.**
- Revocación: v0.1 no recibe el código de cierre `4401` (la librería WebSockets
  no lo expone en el callback). El equipo reintenta con el token anterior y no
  entra solo al portal; la operación local sigue normal.
- **Decisión (v0.1): no se implementa una acción de "borrar identidad" en el
  portal.** Re-provisionar exige el reset de fábrica (plan §11.3), que deja el
  equipo en `PORTAL` y borra WiFi, identidad/`claim_code`, perfiles, canales y
  calibración. Se descarta a propósito el borrado parcial: dejaría el equipo sin
  identidad y sin camino de vuelta al claim sin un reinicio.
- Procedimiento de baja: el backend revoca el token y cierra la sesión; el
  operador hace reset de fábrica, carga WiFi, la URL del backend y un
  `claim_code` nuevo en el portal. Mientras tanto el equipo sigue offline pero
  riega, ventila y controla clima con el perfil local.

## 12. Seguridad

- AP del portal con WPA2; la clave inicial se deriva de la MAC y se muestra en el
  OLED, y el operador puede cambiarla desde el portal. Nunca AP abierto.
- El token no se escribe en logs ni se muestra completo en el OLED.
- Validar todo JSON de entrada: tamaño, tipo, rangos, longitud de strings y
  cantidad de elementos. Rechazar el frame completo ante error; no aplicar
  cambios parciales.
- Límites: 16 KiB por frame, 16 entradas de horario por canal, 16 reglas de
  alarma, 8 canales, 7 días por entrada, strings de hasta 64 caracteres.
- El backend autoriza cada comando según el usuario web; el dispositivo confía en
  el backend, pero aplica sus reglas de seguridad locales (canal habilitado,
  vencimiento, máximos, interlocks).
- **TLS implementado** (handoff del backend): `https://` en `claimDevice()` y
  `wss://` en `startConnection()`, ambos con `setInsecure()` — cifran el tráfico
  pero **no validan la identidad del servidor**. Queda pendiente la validación de
  certificado o pinning antes de considerar el canal endurecido. `ws://` sigue
  permitido para LAN/desarrollo.
- El acceso físico por USB/serial permite reflashear el equipo; el gabinete debe
  cerrarse con llave. No hay secure boot en v0.1.

## 13. Flujos de ejemplo

### 13.1 Arranque con conexión

```
DEV → hello (config_version=7)
SRV → welcome (config_version=8, send_config=true)
SRV → config (v8)
DEV → ack (ref=cfg-0008, ok)
DEV → state
DEV → telemetry
```

### 13.2 Riego remoto

```
SRV → command {id:"cmd-7f3a", channel:"riego_z1", action:"on", duration_s:600,
               expires_at:"...15:06:40Z"}
DEV → ack {ref:"cmd-7f3a", status:"ok", applied:{on:true, until:"...15:16:10Z"}}
DEV → event IRRIGATION_START {reason:"manual_remote"}
DEV → state
...
DEV → event IRRIGATION_STOP {reason:"done"}
DEV → state
```

### 13.3 Corte de Internet

```
DEV → event WS_LOST
DEV: automatización sigue; telemetría y eventos a RAM/NVS
DEV → event ALARM_RAISED (offline_s > 600), si la regla está habilitada
...
DEV → hello (buffered)
DEV → event WS_CONNECTED
DEV → state
DEV → telemetry {buffered:true, samples:[...20]}
DEV → event (pendientes)
```

### 13.4 Entrada al portal

```
DEV → event WS_LOST (se corta STA)
DEV → event PORTAL_ENTER
DEV: automatización sigue con config cacheada
DEV → event PORTAL_EXIT (timeout o guardar)
DEV: reintenta STA y WS; no reinicia
```

## 14. Mínimo que debe implementar el backend

1. Aceptar el upgrade WebSocket con `device_id` + token y una sola sesión.
2. Recibir `hello`, guardar identidad/versiones y responder `welcome`.
3. Persistir `telemetry`, `state` y `event` con deduplicación.
4. Servir `config` versionada cuando el dispositivo esté atrasado.
5. Enviar `command` con `id` único y `expires_at`, y registrar el `ack`.
6. Enviar `ping` cada 25 s y cerrar sesiones mudas.
7. Exponer a la web el estado, la telemetría, los eventos y los comandos.
8. Definir el flujo de alta/token y la autorización de usuarios.

## 15. Decisiones y pendientes con el equipo de backend

| # | Tema | Estado / propuesta |
|---|---|---|
| 1 | Quién define el contrato | **Resuelto:** este proyecto propone este documento; el backend confirma o adapta |
| 2 | Autenticación | header `Authorization: Bearer`; fallback query solo en desarrollo |
| 3 | Alta del dispositivo | **Recomendado:** `claim_code` + `POST /devices/claim` |
| 4 | WSS/TLS | obligatorio antes de salir de LAN/VPN |
| 5 | Frecuencia de telemetría | 30 s normal, ajustable por `welcome.telemetry_s` |
| 6 | Retención de datos | la define el backend (no afecta al firmware) |
| 7 | Hora autoritativa | `server_time` del backend; RTC/NTP como respaldo |
| 8 | Una o varias sesiones | una por dispositivo |
| 9 | Persistencia de la cola | RAM + críticos en NVS en v1 |
| 10 | Actuador de ventana | **Resuelto:** pulso toggle de 5 s, 1 sensor de cerrada; falta datasheet |
| 11 | Config local | **Resuelto:** perfil local editable + override explícito; backend manda por defecto |
| 12 | DS18B20 | **Resuelto:** un pin por sonda (16, 32, 33) |
| 13 | Clave del AP | **Resuelto:** configurable, default derivado de la MAC |
| 14 | Segundo sensor de ventana | v2; requiere GPIO15 boot-safe o expansor I2C |

