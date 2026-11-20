# Tickets — Huecos de API para el backend

> **Para:** equipo de backend (`web1.displaysur.online`).
> **Origen:** §9 de [`frontend.md`](frontend.md), derivado de los requerimientos
> completos del frontend.
> **Estado:** propuesta. Cada ticket está listo para pegarse como issue; los
> caminos y formatos se proponen sobre el contrato de
> [`../02_protocolo_ws.md`](../02_protocolo_ws.md) — si preferís otra forma,
> adaptalo y avisá: el frontend se ajusta.
>
> **Datos de prueba reales:**
> `device_id = vivero-DDAC03` · `fw = 0.1.0-m6` · `claim_code = VIVERO-01`
> · sesión actual `s-d1840c`

---

## T0 — La telemetría no se persiste (P0 · bloqueante)

**Como issue:**

```
Título:  [P0] telemetry no se guarda desde 2026-11-19T05:09:03
Tipo:    bug
```

**Contexto.** El dispositivo envía telemetría cada 30 s y la cola drena
correctamente, pero no aparece en la base. Verificado el 2026-11-20 con
`vivero-DDAC03`:

| Señal | Valor |
|---|---|
| `GET /dashboard/data` → `seriesLen` | **240, sin cambiar** en 4 consultas separadas 40 min |
| `ultimoTs` | **2026-11-19T05:09:03** |
| `latest` | idéntico en todas las consultas |
| `state` / `events` | ✅ sí se actualizan (`last_seen_at` y `WS_CONNECTED` de hoy) |

**Está descartado del lado del firmware:**

- La cola drena: `seq=12 → pendientes=1` y `seq=13 → pendientes=1`.
  `sendTelemetryBatch()` solo hace `popSamples()` **después** de que `sendDoc()`
  devuelve `true`.
- No hay `[WS] ERROR enviando frame` (fallo de envío) ni
  `[WS] error del servidor: ...` (el firmware loguea los `error` entrantes).

**Formato enviado** — idéntico a `02 §5.2`:

```json
{"v":1,"type":"telemetry","id":"m-000123","ts":"2026-11-20T15:00:00Z",
 "payload":{"boot_id":"60ADAF6B","seq":12,"buffered":false,
   "samples":[{
     "ts":"2026-11-20T15:00:00Z",
     "air":{"temp_c":25.4,"rh_pct":57.1,"valid":true},
     "substrate":{"z1":{"moisture_pct":76.7,"temp_c":23.8,"valid":true},
                  "z2":{"moisture_pct":72.3,"temp_c":23.9,"valid":true}},
     "bed":{"temp_c":23.6,"valid":true},
     "window":{"state":"open","limit_closed":false,"moving":false,"direction":null},
     "rssi_dbm":-60,"uptime_s":1800,"time_valid":true}]}}
```

**Criterios de aceptación:**

- [ ] `seriesLen` crece con el tiempo para `vivero-DDAC03`
- [ ] `ultimoTs` avanza (≈30 s por muestra)
- [ ] `latest` refleja la última muestra (los 7 campos, no solo algunos)
- [ ] `hello`, `config` y `event` siguen funcionando igual
- [ ] Reenvío de un lote duplicado no crea filas repetidas
  (dedup por `(device_id, boot_id, seq)`)
- [ ] Test PHPUnit nuevo que cubra el caso

**Nota:** `boot_id` cambia en cada arranque (`esp_random()`), así que la
dedup por `(device_id, boot_id, seq)` no debería colisionar.

---

## T1 — Leer y escribir la configuración (P0)

```
Título:  [P0] API de configuración del dispositivo (GET/PUT)
Bloquea: programación de riego, umbrales, clima, alarmas, canales
```

**Contexto.** Hoy **no existe ningún endpoint** para leer o modificar la
config. El frontend no puede programar riegos ni setear umbrales.

**Petición:**

```http
GET  /api/config?device_id=vivero-DDAC03
PUT  /api/config
Content-Type: application/json
{ ...config... }
```

**Respuesta `GET` (propuesta):**

```json
{
  "device_id": "vivero-DDAC03",
  "config_version": 100,
  "local_version": 4,
  "config_source": "backend",
  "updated_at": "2026-11-20T15:00:00Z",
  "config": {
    "irrigation": {
      "interlock": true,
      "max_run_substrate_s": 900,
      "max_run_sprinkler_s": 1800,
      "min_off_s": 1800,
      "stagger_s": 30,
      "zones": {
        "z1": { "duration_s": 600, "min_humidity_pct": 30,
                "threshold_enabled": true, "paused": false,
                "schedules": [ { "start": "06:00", "duration_s": 600,
                                 "days_mask": 127, "enabled": true } ] },
        "z2": { "...": "idem" },
        "asp": { "duration_s": 600, "paused": false, "schedules": [] }
      }
    },
    "climate": {
      "bed_target_c": 25.0, "bed_auto_enabled": true,
      "vent_auto_enabled": true, "vent_climate_enabled": true,
      "vent_on_temp_c": 30.0, "vent_off_temp_c": 28.0,
      "vent_on_rh_pct": 80.0, "vent_off_rh_pct": 70.0
    },
    "alarms": { "max_temp_c": 38.0, "min_temp_c": 5.0 },
    "channels_enabled": ["riego_z1","riego_z2","aspersion",
                         "ventilacion","ventana","alarma"],
    "timezone_offset_min": -180,
    "backend_url": "https://web1.displaysur.online",
    "ws_url": "",
    "claim_code": "VIVERO-01",
    "ap_password": null
  }
}
```

**`PUT`:** mismo body. Responder `{ "config_version": 101 }` y hacer que el
equipo la reciba por el mensaje `config` del WebSocket (ya funciona: el backend
manda `config` y el equipo lo aplica — verificado con umbrales 45/0 → 38/5 °C).

**Criterios de aceptación:**

- [ ] `GET` devuelve la config **activa** (no la local si `config_source=backend`)
- [ ] `PUT` valida rangos en el servidor; fuera de rango → `422` con `errors[]`
  (el equipo igualmente rechaza con `CONFIG_REJECTED`; conviene filtrar antes)
- [ ] `PUT` incrementa `config_version` y dispara `config` al equipo
- [ ] **Atómico**: una validación falla → no se guarda nada
- [ ] `days_mask` de 0 = horario desactivado (no "todos los días")
- [ ] `ap_password` mínimo 8 caracteres; nunca se devuelve en texto plano
- [ ] Test: guardar → `GET` refleja los cambios → el equipo responde `ack ok`

---

## T2 — Histórico de alarmas (P1)

```
Título:  [P1] Histórico de alarmas con timestamps
Bloquea: pantalla de alarmas (hoy solo hay active/latched en vivo)
```

**Petición:**

```http
GET /api/alarms?device_id=vivero-DDAC03&from=2026-11-19T00:00:00Z&to=&page=1&per_page=50
```

**Respuesta:**

```json
{
  "data": [
    { "condition": "sensor_fault_humedad_z1",
      "label": "Sensor de sustrato Z1 fuera de servicio",
      "severity": "warning", "latched": false,
      "raised_at": "2026-11-19T02:10:00Z",
      "cleared_at": "2026-11-19T04:22:11Z",
      "silenced_until": null,
      "data": { "value": 0 } }
  ],
  "meta": { "page": 1, "per_page": 50, "total": 37 }
}
```

**Catálogo de las 11 condiciones** (claves exactas) — en
[`frontend.md` §6.1](frontend.md). Las de latch son `bed_temp_high`,
`window_fault`, `irrigation_timeout`.

**Criterios de aceptación:**

- [ ] Filtra por rango de fechas y paginación
- [ ] Incluye las alarmas **levantadas y limpiadas** (no solo las vigentes)
- [ ] `latched: true` distinguishible de `active`
- [ ] `silenced_until` presente (silencio ≠ resuelta)
- [ ] Varios `condition` distintos simultáneos para el mismo instante
- [ ] Orden descendente por `raised_at`

---

## T3 — Auditoría paginada y filtrable (P1)

```
Título:  [P1] GET /api/events con filtros y paginación
Bloquea: pantalla de eventos (hoy solo los últimos N)
```

**Petición:**

```http
GET /api/events?device_id=vivero-DDAC03&code=IRRIGATION_START&severity=&channel=&from=&to=&page=1
```

**Respuesta:**

```json
{
  "data": [
    { "event_id": "60ADAF6B-0007", "code": "IRRIGATION_START",
      "severity": "info", "channel": "riego_z1",
      "ts": "2026-11-20T15:00:12Z", "boot_id": "60ADAF6B",
      "buffered": false, "message": "Riego Z1 iniciado por horario",
      "data": { "reason": "schedule", "duration_s": 600, "entry_id": "z1-1" } }
  ],
  "meta": { "page": 1, "per_page": 50, "total": 412 }
}
```

**Criterios de aceptación:**

- [ ] **Deduplicar por `event_id`** — los eventos críticos se reenvían tras un
      reinicio con su `boot_id` original y pueden repetirse en el tiempo
- [ ] Filtros: `code`, `severity`, `channel`, rango de fechas
- [ ] Paginación con `meta.total`
- [ ] Orden descendente por `ts`
- [ ] Los ~40 códigos de `02 §5.4` se devuelven tal cual (no traducidos)

---

## T4 — Estado de los comandos y su `ack` (P1)

```
Título:  [P1] GET /api/commands con estado del ack
Bloquea: feedback de control en la UI
```

**Petición:**

```http
GET /api/commands?device_id=vivero-DDAC03&status=&page=1
```

**Respuesta:**

```json
{
  "data": [
    { "id": "cmd-7f3a", "channel": "riego_z1", "action": "on",
      "duration_s": 600, "requested_by": "web:admin",
      "created_at": "2026-11-20T15:00:00Z", "expires_at": "...",
      "sent_at": "2026-11-20T15:00:01Z",
      "ack_status": "acked", "ack_error": null,
      "acked_at": "2026-11-20T15:00:02Z" }
  ],
  "meta": { "page": 1, "per_page": 50, "total": 12 }
}
```

`ack_status`: `pending` | `acked` | `expired` | `rejected`.
`ack_error` cuando `rejected`: uno de `local_manual_active`, `channel_disabled`,
`interlock`, `safety_interlock`, `unsupported`, `invalid_payload`, `duplicate`.

**Criterios de aceptación:**

- [ ] `POST /dashboard/command` devuelve el `id` para poder consultar después
- [ ] El `ack` real del equipo se escribe en la fila (no solo `acked` por
      asumir)
- [ ] Comando vencido → `expired`, sin reintentar
- [ ] Reenvío del mismo `id` → mismo resultado, sin repetir acción

---

## T5 — Lista de dispositivos con estado (P0)

```
Título:  [P0] GET /api/devices con estado resumido
Bloquea: selector de dispositivo (hoy se elige a ciegas)
```

**Contexto.** Hoy hay **4 dispositivos** y el `<select>` solo muestra IDs.
El usuario elige sin saber cuál está online ni cuál tiene alarmas.

**Petición y respuesta:**

```http
GET /api/devices
```

```json
{
  "data": [
    { "device_id": "vivero-DDAC03", "name": "Vivero principal",
      "online": true, "last_seen_at": "2026-11-20T15:18:42Z",
      "mode": "NORMAL", "config_source": "backend",
      "fw_version": "0.1.0-m6", "rssi_dbm": -60,
      "active_alarms": 0, "latched_alarms": 1,
      "ip": "192.168.1.8" }
  ]
}
```

**Criterios de aceptación:**

- [ ] `online` calculado con `now - last_seen_at > 90 s` (o el umbral que
      definaís; el equipo manda `state` cada 5 min y heartbeat cada 25 s)
- [ ] `active_alarms` / `latched_alarms` para pintar el badge sin pedir más
- [ ] Orden: online primero, luego por `last_seen_at` descendente
- [ ] Incluye los `demo-*` con un flag `demo: true` para poder filtrarlos

---

## T6 — Diagnóstico del equipo (P2)

```
Título:  [P2] GET /api/diagnostics
Bloquea: pantalla de soporte
```

```http
GET /api/diagnostics?device_id=vivero-DDAC03
```

```json
{
  "device_id": "vivero-DDAC03",
  "boot_id": "60ADAF6B",
  "fw_version": "0.1.0-m6",
  "uptime_s": 1800,
  "reconnects": 3,
  "queues": { "telemetry_pending": 0, "events_pending": 0,
              "critical_events": 1 },
  "last_error": "",
  "clock": { "source": "rtc", "valid": true, "utc": "2026-11-20T15:00:00Z" },
  "wifi": { "connected": true, "rssi_dbm": -60, "ip": "192.168.1.8" },
  "ws": { "connected": true, "session_id": "s-d1840c", "last_rx_s": 3 }
}
```

**Cómo obtenerlo:** el firmware ya responde `request {"what":"diagnostics"}`
con un `event DIAGNOSTICS` (heap, tareas, uptime, reconexiones, versión).
El backend puede pedirlo bajo demanda y cachearlo.

**Criterios de aceptación:**

- [ ] Dispara `request {"what":"diagnostics"}` al equipo y devuelve la respuesta
- [ ] Responde con datos en caché si el equipo está offline (con `stale: true`)
- [ ] Incluye `queues.telemetry_pending` (útil para el incidente T0)

---

## T7 — Telemetría por rango y métricas (P1)

```
Título:  [P1] GET /api/telemetry con rango y selección de métricas
Bloquea: gráficos con rangos (hoy solo 'duration' fijo)
```

**Petición:**

```http
GET /api/telemetry?device_id=vivero-DDAC03
                  &from=2026-11-19T00:00:00Z&to=2026-11-20T00:00:00Z
                  &metrics=air_temp_c,substrate1_moisture_pct
                  &interval=15m
```

**Respuesta:**

```json
{
  "data": [ { "ts": "2026-11-19T12:00:00Z",
              "air_temp_c": 24.9, "substrate1_moisture_pct": 76.7 } ],
  "meta": { "from": "...", "to": "...", "count": 96, "interval": "15m" }
}
```

**Criterios de aceptación:**

- [ ] `interval` agrega (downsampling) para no devolver miles de filas
- [ ] `metrics` selecciona columnas (respeta el `null` de las inválidas)
- [ ] Acepta `ts` antiguos y fuera de orden (el equipo bufferiza hasta 6 h)
- [ ] Límite máximo de filas documentado y respetado
- [ ] Devuelve `buffered` cuando aplique

---

## T8 — `POST /dashboard/command` debe devolver el `ack` (P0)

```
Título:  [P0] /dashboard/command sin feedback de ack
Bloquea: confirmación de acciones en la UI
```

**Contexto.** Hoy el endpoint dispara el comando pero la UI no sabe si el
equipo lo aceptó. Necesita la respuesta del `ack` real, no solo un `200`.

**Respuesta propuesta:**

```json
{ "command_id": "cmd-7f3a", "status": "sent", "queued_at": "..." }
```

y el estado final por `GET /api/commands/{id}` (T4) o por websocket.

**Criterios de aceptación:**

- [ ] Devuelve `command_id` para poder consultar después
- [ ] Rechazo inmediato (canal deshabilitado, `interlock`) → `409` con la razón
- [ ] La UI puede distinguir "enviado" de "el equipo lo ejecutó"

---

## Orden sugerido

| | Ticket | Por qué primero |
|---|---|---|
| 1 | **T0** | sin telemetría no hay nada que leer |
| 2 | **T1** | sin config no hay riego ni umbrales: el 80 % del producto |
| 3 | **T5** | el selector a ciegas rompe todo lo demás |
| 4 | **T8 + T4** | sin ack no hay feedback de control |
| 5 | **T2 + T3** | alarmas y auditoría |
| 6 | **T7** | gráficos con rangos |
| 7 | **T6** | soporte |

---

## Fuera de este lote (decisiones, no tickets)

- **WSS/TLS:** ya implementado en el firmware con `setInsecure()`. Queda decidir
  validación de certificado o pinning (handoff §5).
- **Retención de datos** en MariaDB.
- **Roles de usuario**: ¿el operador puede editar config o solo administrador?
- **`POST /api/v1/devices/claim`** y headers del WS: ya funcionan, solo
  documentarlos como estables.
