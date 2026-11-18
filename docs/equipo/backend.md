# Requerimientos — Backend

**Alcance:** API HTTP + servidor WebSocket + MariaDB, para exponer el estado
del vivero a la web y enviarle configuración/comandos al dispositivo.

**Contrato:** [`../02_protocolo_ws.md`](../02_protocolo_ws.md) es la fuente de
verdad. Este documento complementa con lo que no está ahí (HTTP de la web,
modelo de datos, pruebas).

---

## 1. Mínimo viable

De `02 §14`, lo que hay que implementar para que el sistema funcione:

1. Aceptar el upgrade WebSocket con `device_id` + token y **una sola sesión**
   por dispositivo.
2. Recibir `hello`, guardar identidad/versiones y responder `welcome`.
3. Persistir `telemetry`, `state` y `event` **con deduplicación** (por
   `seq`, `event_id`).
4. Servir `config` **versionada** cuando el dispositivo esté atrasado.
5. Enviar `command` con `id` único y `expires_at`, y registrar el `ack`.
6. Enviar `ping` cada 25 s y cerrar sesiones mudas.
7. Exponer a la web estado, telemetría, eventos y comandos.
8. Definir el flujo de alta/token y la autorización de usuarios.

---

## 2. HTTP

### 2.1 Alta del dispositivo (obligatorio)

```http
POST /api/v1/devices/claim
Content-Type: application/json

{
  "device_id": "vivero-1a2b3c",
  "claim_code": "VIVERO-01",
  "fw_version": "0.1.0-m6",
  "hw_revision": "v1"
}
```

**200**
```json
{ "token": "…", "ws_url": "ws://192.168.1.50:8000/ws/device", "config_version": 100 }
```

- `403` si `claim_code` es inválido o ya se usó.
- El dispositivo reintenta **60 s** si no hay red/HTTP y **5 min** si el
  backend rechaza el código. **No entra solo al portal** por un fallo de
  claim: sigue operando offline.
- El `claim_code` se guarda en el dispositivo y solo se cambia desde su
  portal.

> **Pendiente de confirmar:** ruta exacta y si se acepta el `claim_code` de
> una sola vez o por dispositivo. El firmware ya llama a
> `POST /api/v1/devices/claim` (`WS_CLAIM_PATH` en `src/config.h`).

### 2.2 Superficie REST para la web (propuesta)

El frontend necesita como mínimo esto. Es una **propuesta**: el backend define
rutas y auth, pero el contenido debe cubrir estos datos.

| Método | Ruta | Qué devuelve |
|---|---|---|
| `GET` | `/api/devices` | lista de dispositivos con estado de conexión |
| `GET` | `/api/devices/:id` | identidad, `fw_version`, RSSI, uptime, modo, `config_source` |
| `GET` | `/api/devices/:id/telemetry?from=&to=&limit=` | serie temporal (1 muestra cada 30 s por defecto) |
| `GET` | `/api/devices/:id/state` | último `state` recibido |
| `GET` | `/api/devices/:id/events?code=&severity=&from=&to=` | auditoría |
| `GET` | `/api/devices/:id/alarms` | alarmas activas + latched |
| `POST` | `/api/devices/:id/commands` | encola un `command`; devuelve su `id` |
| `GET` | `/api/devices/:id/commands/:id` | estado del comando y su `ack` |
| `GET`/`PUT` | `/api/devices/:id/config` | config versionada |

**Autenticación:** `Authorization: Bearer <token>` en el WS y en la API.
Los usuarios de la web son un tema del backend (§15.2).

---

## 3. Servidor WebSocket

- Ruta: `/ws/device`.
- Headers esperados en el upgrade:
  - `X-Device-Id: <device_id>`
  - `Authorization: Bearer <token>`
- **Una sesión por dispositivo**: una segunda sesión debe cerrar la anterior
  (o rechazarla con `4409`).
- Fallback aceptable en desarrollo: query `?device_id=&token=` en la URL.
  Menos seguro (el token queda en logs); no usar en producción.
- Texto JSON, un mensaje por frame, UTF-8.
- **v0.1 no soporta subprotocolo** (`Sec-WebSocket-Protocol`): el firmware
  envía la cabecera vacía. No exigir subprotocolo.
- Heartbeat: `ping` del servidor cada 25 s; si no hay actividad, cerrar.
- El firmware **no reconecta automáticamente rápido**: backoff de 2 s a 60 s
  y decide él mismo. No saturar con reintentos.

Detalle completo en `02 §2`, `§3`, `§9`.

### 3.1 Manejo de los mensajes que llegan

| Mensaje | Qué hacer | Cuidado |
|---|---|---|
| `hello` | identificar el equipo, guardar `fw_version`, versiones y **`capabilities`** | `enabled_channels` es local: mostrar, no modificar |
| `telemetry` | insertar en la serie temporal | **deduplicar por `seq`**; acepta lotes y `ts` antiguos |
| `state` | sobrescribir el último estado del dispositivo | es un **snapshot**, no una serie |
| `event` | insertar en la auditoría | **deduplicar por `event_id`** — los críticos se reenvían tras un reinicio |
| `ack` | actualizar el comando correspondiente | puede llegar mucho después de `expires_at` |
| `pong` | medir latencia y detectar sesión muerta | |

#### Captación de datos: el caso del lote

El equipo puede estar horas desconectado. Al reconectar manda **hasta 720
muestras (≈6 h)** en **lotes de hasta 20**, con `buffered: true`. Por eso:

1. **Insertar sin asumir tiempo real** — aceptar `ts` de horas atrás.
2. **Deduplicar por `(device_id, seq)`** — un reintento reenvía el mismo lote.
3. **Marcar `buffered`** para que la web lo distinga de "en vivo".
4. **No descartar por "llegó tarde"**: ese mecanismo es el respaldo del sistema
   cuando se corta Internet (`02 §9.2`).

Los eventos siguen la misma lógica: se envían de a uno en orden cronológico, y
los **críticos** (`ALARM_RAISED`, `CONFIG_REJECTED`, `WATCHDOG_RESET`,
`NVS_ERROR`) se persisten en NVS del equipo y se reenvían tras reiniciar con su
`boot_id` **original**. Deduplicar por `event_id`, nunca por `ts`.

#### `capabilities`: qué puede hacer el equipo

`hello.capabilities` (`02 §5.1`) es lo que le dice a la web qué puede pedir:

- `channels` y `enabled_channels` → qué controles ofrecer;
- `window_actuator: pulse_toggle_5s` → la ventana es **toggle**, no tiene
  estado continuo;
- `window_stop: false` → **no existe la acción `stop`** (responde `unsupported`);
- `window_limits: 1` → solo confirma "cerrada".

Guardalo con el dispositivo y servilo en `GET /api/devices/:id`: la web no debe
adivinar estas restricciones.

#### Estado: snapshot, no serie

`state` llega al conectar, cada ~5 min y al cambiar algo. Se **sobrescribe** (el
último gana); para el histórico de estado conviene usar los `eventos`. Guardá
también `channels[].source` y `channels[].until`: son los que le dicen a la web
**quién tiene el control** de cada actuador (`auto`, `manual_local`,
`manual_remote`, `test`, `off`) y cuándo vence un comando remoto.

---

## 4. Modelo de datos sugerido

Solo como guía. El backend diseña el esquema; lo que sigue es lo que el
contrato implica que hay que guardar.

| Entidad | Campos mínimos | Notas |
|---|---|---|
| `devices` | `device_id` (PK), `claim_code_hash`, `token_hash`, `fw_version`, `hw_revision`, `last_seen_at`, `last_ip` | alta por claim |
| `device_configs` | `device_id`, `version`, `config` (JSON), `created_at` | **versionada**: `version` es lo que viaja en `config.version` |
| `telemetry` | `device_id`, `seq`, `ts`, métricas | **deduplicar por `(device_id, seq)`** |
| `device_state` | `device_id`, `ts`, `mode`, `config_source`, `window_state`, `active_alarms`, … | último estado (no serie) |
| `events` | `device_id`, `event_id` (PK), `ts`, `code`, `severity`, `channel`, `message`, `data`, `boot_id`, `buffered` | **deduplicar por `event_id`** |
| `irrigation_events` | `device_id`, `client_event_id`, zona, `trigger`, `started_at`, `ended_at`, `suppressed` | historial de riego; el dispositivo envía `client_event_id` para idempotencia |
| `commands` | `id`, `device_id`, `channel`, `action`, `payload`, `requested_by`, `expires_at`, `sent_at`, `ack_status`, `ack_error` | el `ack` se escribe al volver |
| `users` / `sessions` | auth de la web | fuera del contrato |

**Retención:** la define el backend (`02 §15.6`). Convendría separar
telemetría (serie, se archiva) de eventos (auditoría, se conserva).

---

## 5. Catálogos

### 5.1 Alarmas (`AlarmCondition`)

El dispositivo evalúa **11 condiciones** cada 30 s. La API debe exponer estas
claves tal cual:

| Clave | Severidad | Latch por defecto | Crítica | Condición |
|---|---|---|---|---|
| `temp_max` | warning→critical | no | ✅ | temperatura de aire > máximo |
| `temp_min` | warning→critical | no | ✅ | temperatura de aire < mínimo |
| `sin_conexion` | warning | no | ❌ | sin contacto con backend > 60 min |
| `humedad_persistente_baja` | warning | no | ❌ | sustrato bajo el mínimo 60 min seguidos |
| `bed_temp_high` | critical | **sí** | ✅ | cama ≥ `max_c` (35 °C) |
| `window_fault` | warning | **sí** | ✅ | ventana en `FAULT` |
| `sensor_fault_aire` | warning | no | ❌ | DHT21 inválido 3 ciclos seguidos |
| `sensor_fault_humedad_z1` | warning | no | ❌ | humedad o temperatura Z1 inválida 3 ciclos |
| `sensor_fault_humedad_z2` | warning | no | ❌ | idem Z2 |
| `sensor_fault_cama` | warning | no | ❌ | DS18B20 de cama inválido 3 ciclos |
| `irrigation_timeout` | warning | **sí** | ✅ | riego cortado por `max_run_s` |

Comportamiento a respetar en la API:

- Las condiciones son **independientes y simultáneas**: el backend debe
  manejar **más de una a la vez** (no un único "alarma activa").
- **Latch**: `bed_temp_high`, `window_fault` y `irrigation_timeout` quedan
  pegadas hasta `command alarma / reset`. El resto se limpia solo.
- **Silencio**: apaga el relé 10 min sin borrar condiciones; una condición
  **crítica nueva** durante el silencio lo cancela.
- El firmware distingue `active` (condición presente) de `latched`.

### 5.2 Eventos

Códigos, severidad y `data` en `02 §5.4`. Es la lista completa y la fuente
de verdad — no la dupliques en código: importala desde el contrato.

Grupos que la web casi seguro va a usar:

- **Cadena de agua:** `IRRIGATION_START`, `IRRIGATION_STOP`,
  `IRRIGATION_SKIPPED`, `SCHEDULE_MISSED`
- **Alarmas:** `ALARM_RAISED`, `ALARM_CLEARED`, `ALARM_SILENCED`
- **Ventana:** `WINDOW_OPEN`, `WINDOW_CLOSE`, `WINDOW_TIMEOUT`,
  `WINDOW_FAULT`
- **Clima:** `THERMOSTAT_ON/OFF`, `LIGHT_ON/OFF`, `FAN_ON/OFF`
- **Config:** `CONFIG_APPLIED`, `CONFIG_REJECTED`, `CONFIG_SOURCE_CHANGED`
- **Firmware:** `BOOT`, `WATCHDOG_RESET`, `NVS_ERROR`, `SENSOR_FAULT`,
  `SENSOR_RECOVERED`, `COMMAND_REJECTED`

**Deduplicación:** `event_id = <boot_id>-<secuencia>`. Los eventos críticos
restaurados desde NVS conservan el `boot_id` **original** (no el del arranque
actual), para que la deduplicación siga siendo válida. El backend debe
deduplicar por `event_id`, no por `ts`.

### 5.3 Comandos

| Canal | Acciones | Campos |
|---|---|---|
| `riego_z1`, `riego_z2`, `aspersion` | `on`, `off` | `duration_s` **obligatorio** en `on` (1…`max_run_s`) |
| `iluminacion`, `ventilacion` | `on`, `off` | `duration_s` opcional; default 600 s |
| `cama_caliente` | `on`, `off`, `auto` | `duration_s` opcional (default 1800 s, máx 14400); `auto` vuelve al termostato |
| `ventana` | `open`, `close` | sin `duration_s`; pulso de 5 s. `stop` **no soportado** en v1 → `unsupported` |
| `alarma` | `silence`, `reset` | `duration_s` en `silence` (1…`silence_max_s`); `reset` limpia latches |

Restricciones que el backend debe conocer (el dispositivo las aplica y puede
rechazar):

- Un comando con `expires_at` ya vencido se responde `expired` y **no actúa**.
- `command.id` se deduplica (8 entradas, 10 min): reenviar el mismo `id` da el
  mismo `ack`, sin repetir la acción.
- **Prioridad local:** si el operador en el equipo tiene un canal en manual
  local, el `on` remoto se responde `local_manual_active`. El `off` siempre se
  acepta (es la acción segura).
- Interlock de riego: solo un riego a la vez; si está ocupado, se responde
  `interlock`.

---

## 6. Pruebas

```powershell
python tools\fake_backend.py --selftest
```

Cubre: claim 403/200, cierre `4401`, `welcome`/`config`, telemetría, `state`,
`event`, `command`/`ack`, `ping`/`pong`, `request`.

**Ese script es tu especificación ejecutable.** Si tu implementación responde
igual que él ante los mismos mensajes, el firmware va a funcionar.

Casos de prueba del sistema completo: `../03_reglas_y_plan.md` §14.3 (T18–T31
son los del WebSocket).

---

## 7. Pendientes que hay que cerrar

De `02 §15`. Los que te afectan directamente:

| # | Tema | Estado |
|---|---|---|
| 2 | Autenticación `Authorization: Bearer`; fallback query solo en dev | por confirmar |
| 3 | Alta por `claim_code` + `POST /devices/claim` | **recomendado**, falta ruta exacta |
| 4 | **WSS/TLS** obligatorio antes de salir de LAN/VPN | por implementar |
| 5 | Frecuencia de telemetría (30 s default, ajustable con `welcome.telemetry_s`) | por definir |
| 6 | Retención de datos | por definir |
| 7 | Hora autoritativa: `server_time` del backend; RTC/NTP como respaldo | por definir |
| 8 | Una sesión por dispositivo | decidido |

**También hay que confirmar si el servidor WS soporta headers en el upgrade**
(`X-Device-Id`, `Authorization`). Si no, el firmware usa el fallback por query
— pero eso hay que decirlo, no descubrirlo en producción.
