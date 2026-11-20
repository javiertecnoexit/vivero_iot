# Informe de requerimientos — Frontend del Vivero IoT

> **Para:** equipo de frontend.
> **Fecha:** 2026-11-20. **Firmware:** `0.1.0-m6`. **Backend:** `web1.displaysur.online`.
> **Estado del dashboard actual:** MVP que sirvió para verificar la comunicación con
> el hardware. **Este documento es lo que falta para el producto completo.**
> Todo lo que está verificado en producción está marcado ✅; lo propuesto, ⬜.

---

## 0. Resumen ejecutivo

**Lo que ya funciona de punta a punta** (verificado con el equipo real `vivero-DDAC03`):

- Login de sesión, selector de dispositivo, 4 dispositivos en producción
- `GET /dashboard/data` devuelve estado en vivo, serie temporal, eventos y capacidades
- El equipo se conecta por `wss://`, manda telemetría cada 30 s y recibe `config`
- `POST /dashboard/command` envía comandos y el equipo los ejecuta
- Las capacidades del equipo (`window_stop: false`, canales habilitados) llegan y se guardan

**Lo que falta para el producto** (los puntos 3 a 10 de este informe):

| Área | Estado |
|---|---|
| Programación de riego (horarios, días, umbrales) | ⬜ **sin API** |
| Edición de configuración del equipo | ⬜ **sin API** |
| Histórico de alarmas | ⬜ solo `active`/`latched` en vivo |
| Histórico de eventos paginado/filtrable | ⬜ solo los últimos N |
| Historial de comandos y su `ack` | ⬜ sin endpoint de consulta |
| Diagnóstico del equipo (colas, boot_id, errores) | ⬜ parcial en `state` |
| Gestión de usuarios/roles | ⬜ sin API |

---

## 1. Superficie real del API (verificada el 2026-11-20)

### 1.1 Existe

| Método | Ruta | Auth | Devuelve |
|---|---|---|---|
| `GET` | `/health` | pública | `{"ok":true,"service":"vivero-backend","claim_code":"VIVERO-01","devices":4}` |
| `GET` | `/login` | — | formulario (Blade + CSRF `_token`) |
| `POST` | `/logout` | sesión | — |
| `GET` | `/dashboard` | sesión | HTML con el componente Alpine |
| `GET` | `/dashboard/data?device=<id>` | sesión | **todo el estado** (§2) |
| `POST` | `/dashboard/command` | sesión | dispara un `command` al equipo |
| `POST` | `/api/v1/devices/claim` | `claim_code` | alta del equipo (lo hace el firmware) |

> Sin `?device=`, `/dashboard/data` devuelve **el primer dispositivo** de la lista.
> **Siempre enviar el `device_id` explícito.**

### 1.2 NO existe (devuelven 404, verificado)

```
/dashboard/devices   /dashboard/alarms   /dashboard/events   /dashboard/config
/devices   /alarms   /events   /config   /settings   /telemetry   /state
/api/v1/devices/{id}   /api/v1/devices/{id}/telemetry|state|events|alarms|config
```

> Los últimos figuran en el handoff del backend como "REST de apoyo", pero **hoy
> no están implementados**. Es el punto 9 de este informe.

---

## 2. `GET /dashboard/data` — esquema completo

Respuesta real (acortada) para `vivero-DDAC03`:

```json
{
  "device_id": "vivero-DDAC03",
  "online": true,
  "mode": "NORMAL",
  "config_source": "backend",
  "fw_version": "0.1.0-m6",
  "last_seen_at": "2026-11-20T15:18:42+00:00",
  "last_ip": "192.168.1.8",
  "rssi_dbm": -60,
  "uptime_s": 5403,
  "now": "2026-11-20T15:19:00+00:00",

  "latest":  { "air_temp_c": "24.9", "air_rh_pct": "61.30",
               "substrate1_moisture_pct": "97.91", "substrate2_moisture_pct": null,
               "substrate1_temp_c": "23.3", "substrate2_temp_c": null,
               "bed_temp_c": "24.1", "window_state": "open",
               "rssi_dbm": -66, "uptime_s": 5403 },

  "series":  [ { "ts": "...", "air_temp_c": ..., "air_rh_pct": ...,
                 "substrate1_moisture_pct": ..., "substrate2_temp_c": ...,
                 "bed_temp_c": ..., "window_state": "open" }, ... ],

  "state":   { "mode": "NORMAL", "config_source": "backend",
               "config_version": 100, "local_version": 4,
               "clock": { "utc": "...", "source": "rtc", "valid": true,
                          "timezone_offset_min": -180 },
               "wifi":  { "connected": true, "rssi_dbm": -56, "ip": "192.168.1.8" },
               "window": { "state": "open", "moving": false, "direction": null,
                           "fault": false, "last_move_ts": null },
               "alarms": { "active": [], "latched": [], "silenced_until": null },
               "channels": { "riego_z1": { "enabled": true, "on": false,
                              "source": "off", "until": null }, ... } },

  "events":  [ { "code": "WS_CONNECTED", "severity": "info", "ts": "...",
                 "channel": null, "message": "WebSocket conectado" }, ... ],

  "capabilities": { "channels": [...8], "enabled_channels": [...6],
                    "window_actuator": "pulse_toggle_5s",
                    "window_limits": 1, "window_stop": false },

  "enabled_channels": ["riego_z1", "riego_z2", "aspersion",
                       "ventilacion", "ventana", "alarma"]
}
```

### Notas de implementación

- **Los números llegan como `string`** (`"24.9"`, `"97.91"`). Convertir con
  `Number()` antes de graficar; `null` → "sin dato".
- **`latest` y `series` usan nombres planos** (`substrate1_moisture_pct`); el
  contrato del wire (`02 §5.2`) es anidado (`substrate.z1.moisture_pct`). El
  backend ya aplana: **usar los planos**.
- **`state` es un snapshot**, se sobrescribe. Para histórico usar `events`.
- **`series` puede traer datos viejos.** El 2026-11-20 traía muestras del
  19/11: verificar `ts` contra `now` antes de pintar "en vivo".
- `duration` (min) se envía como query param; el MVP lo trae fijo en `600`.

---

## 3. Pantallas requeridas

### 3.1 Sesión y navegación ⬜

- Login (ya existe), logout, y **persistir la sesión** (hoy caduca y vuelve al login).
- Selector de dispositivo con **estado resumido** (online/offline, alarmas, último
  contacto). Hoy el `<select>` solo lista IDs — un usuario elige a ciegas.
  Conviene exponer una lista con estado.

### 3.2 Panel general ✅ parcial

| Bloque | Estado | Falta |
|---|---|---|
| Online / último contacto | ✅ | indicar si `now - last_seen_at > 90 s` → "desconectado" |
| Modo y fuente de config | ✅ | avisar si `config_source: local` (override del operador) |
| Tarjetas de métricas | ✅ | **respetar `null`** (§6.2) |
| Gráficos temperatura/humedad | ✅ | rangos seleccionables (1 h / 24 h / 7 d) |
| Canales / actuadores | ✅ | mostrar `source` y `until` (§5) |
| Controles | ✅ | feedback de `ack` (§5.3) |
| Alarmas | ⬜ | falta histórico, latched con badge, botón reset |
| Eventos | ⬜ | falta filtro por `code`/`severity`/fecha y paginación |

### 3.3 Detalle de dispositivo ⬜

- `fw_version`, `boot_id`, `uptime_s`, IP, RSSI, `config_version`/`local_version`
- **Diagnóstico:** estado WS, cola de telemetría pendiente, último error, número
  de reconexiones. El firmware los tiene (`wsPendingSamples`, `wsLastError`,
  `wsReconnectCount`) — hay que pedirlos al backend.
- **Capacidades** (`capabilities`): qué puede hacer el equipo. Sin esto la UI
  ofrece controles que el equipo rechaza.

### 3.4 Riego — programación y umbrales ⬜ **(sin API)**

Esta es la funcionalidad central y **no hay endpoint para leer ni escribir config**.

**Por zona (Z1, Z2, aspersión):**

| Campo | Rango | Notas |
|---|---|---|
| `duration_s` | 1…3600 | duración base del riego |
| `min_humidity_pct` | 0…100 | umbral para riego automático (Z1/Z2) |
| `threshold_enabled` | bool | si está en `false`, no riega por umbral |
| `paused` | bool | pausa la automatización; **el manual sigue** |
| `schedules[4]` | | 4 horarios por zona |
| ↳ `start` | `HH:MM` | hora local (UTC-3 por defecto) |
| ↳ `duration_s` | 1…3600 | duración de esa entrada |
| ↳ `days_mask` | 7 bits, lunes=bit0 | **los 7 desmarcados = horario desactivado** |
| ↳ `enabled` | bool | |

**Globales:**

| Campo | Rango | Notas |
|---|---|---|
| `interlock` | bool | un solo riego a la vez (Z1+Z2+aspersión) |
| `max_run_substrate_s` | 1…3600 | corte duro; al disparar → `irrigation_timeout` |
| `max_run_sprinkler_s` | 1…3600 | |
| `min_off_s` | 0…43200 | espera entre ciclos automáticos por umbral |
| `stagger_s` | 0…600 | espera entre zonas distintas |
| `timezone_offset_min` | −720…840 | default −180 |

**Validación obligatoria en la UI** (si no, el equipo responde `CONFIG_REJECTED`
con `data.errors` y **no aplica nada** — es atómico):

- Duración 1…3600; `min_off_s` ≤ 43200; `stagger_s` ≤ 600
- Umbral 0…100 %
- Un horario con **los 7 días desmarcados queda desactivado**
- Hora en `HH:MM` de 24 h

### 3.5 Clima ⬜ **(sin API)**

| Campo | Rango | Notas |
|---|---|---|
| `bed_target_c` | 5…34.9 | setpoint de la cama; el corte duro a **35 °C** no es configurable |
| `bed_auto_enabled` | bool | si `false`, la cama no arranca sola |
| `vent_auto_enabled` | bool | |
| `vent_climate_enabled` | bool | rige por temperatura/humedad; si `false`, solo por horario |
| `vent_on_temp_c` / `vent_off_temp_c` | −20…80 | histéresis: enciende ≥ `on`, apaga ≤ `off` |
| `vent_on_rh_pct` / `vent_off_rh_pct` | 0…100 | lo mismo con humedad |
| `alarm_max_temp_c` | −20…80 | |
| `alarm_min_temp_c` | −50…40 | |

**Las dos condiciones de ventilación son OR** (temperatura **o** humedad) y
usan **2 ciclos consecutivos** de confirmación (60 s).

### 3.6 Configuración del equipo ⬜ **(sin API)**

| Campo | Notas |
|---|---|
| `enabled_channels[8]` | solo habilitar/deshabilitar; **no se puede cambiar la función ni el GPIO** |
| `backend_url`, `ws_url` | con `https://` / `wss://` soportados desde el firmware `0.1.0-m6` |
| `claim_code` | alta en el backend |
| `ap_password` | mínimo 8 caracteres |
| `config_source` | `backend` (default) o `local` (override) |

> **Mostrar siempre `config_source`.** Si es `local`, los cambios del backend se
> guardan pero **no se aplican** — la UI debe decirlo o el operador pensará que
> su edición no sirve.

---

## 4. Datos de telemetría — lectura correcta

### 4.1 Campos

| Campo | Unidad | Fuente |
|---|---|---|
| `air_temp_c` | °C | DHT21 |
| `air_rh_pct` | % | DHT21 |
| `substrate1_moisture_pct` / `substrate2_moisture_pct` | % | ADC GPIO34/35 |
| `substrate1_temp_c` / `substrate2_temp_c` | °C | DS18B20 GPIO16/32 |
| `bed_temp_c` | °C | DS18B20 GPIO33 |
| `window_state` | enum | `closed` / `open` / `moving` / `fault` / `unknown` |
| `rssi_dbm` | dBm | WiFi |
| `uptime_s` | s | firmware |

### 4.2 Reglas de presentación

1. **`null` → "sin dato", nunca `0`.** Es el error más fácil de cometer.
2. **No asumir tiempo real.** El equipo bufferiza **720 muestras (~6 h)** y las
   envía en lotes de hasta 20 con `buffered: true`. Aceptar `ts` antiguo y
   fuera de orden; deduplicar por `seq`, no por `ts`.
3. **Marcar la antigüedad.** Si `now - latest.ts > 2 ciclos`, mostrar "última
   lectura hace X min" en vez de un valor aparentemente en vivo.
4. **La humedad necesita calibración.** Sin calibrar usa `seco=3000 / húmedo=1200`
   y el número no significa nada. Indicar "sin calibrar" si el equipo lo dice.
5. **`window_state` no es posición absoluta:** en v1 solo hay sensor de *cerrada*
   (`window_limits: 1`). `open` significa "el sensor no indica cerrada".

### 4.3 Incidencia abierta (bloqueante para la lectura)

> **La telemetría no se está persistiendo desde `2026-11-19T05:09:03`.**
> Verificado el 2026-11-20: `seriesLen` fijo en 240 y `latest` sin moverse en
> 40 min, mientras `state` y `events` sí se actualizan. El equipo la envía
> (la cola drena y no hay errores de envío ni `error` del servidor).
> **Mientras eso no se arregle, la serie temporal y `latest` muestran datos
> viejos.** Informe entregado al equipo de backend.

---

## 5. Control de actuadores

### 5.1 Canales y acciones

| Canal | Acciones | Campos |
|---|---|---|
| `riego_z1`, `riego_z2`, `aspersion` | `on`, `off` | **`duration_s` obligatorio** en `on` (1…`max_run_s`) |
| `iluminacion`, `ventilacion` | `on`, `off` | `duration_s` opcional (default 600 s) |
| `cama_caliente` | `on`, `off`, `auto` | `duration_s` opcional (default 1800, máx 14400); `auto` devuelve al termostato |
| `ventana` | `open`, `close` | sin duración; **`stop` NO existe** en v1 → `unsupported` |
| `alarma` | `silence`, `reset` | `duration_s` en `silence`; `reset` limpia latches |

### 5.2 `source`: quién tiene el control

`state.channels.<id>.source` es **clave** para no confundir al operador:

| Valor | Quién mandó | Qué mostrar |
|---|---|---|
| `off` | nadie | "apagado" |
| `auto` | automatización del equipo (horario/umbral/termostato) | "automático" |
| `manual_local` | alguien en el equipo (botones u OLED) | **"control local"** — la web no puede encender |
| `manual_remote` | un comando de esta web | "control remoto" con cuenta regresiva de `until` |
| `test` | prueba de canal desde el portal del equipo | "en prueba (30 s)" |

**La prioridad local es diseño, no error** (`03 §1.1`). Con `source:
manual_local`, un `on` remoto responde `local_manual_active`. Mostrarlo como
estado, no como fallo.

### 5.3 Estados del `ack`

| `status` | Qué mostrar |
|---|---|
| `ok` | aceptado; usar `on`/`until` del resultado |
| `expired` | "el comando venció, reintentá" |
| `duplicate` | reintento: mostrar el resultado original, **no reenviar** |
| `local_manual_active` | "alguien en el equipo tiene el control manual" |
| `channel_disabled` | "canal deshabilitado en el equipo" |
| `interlock` | "hay otro riego en curso" |
| `safety_interlock` | rechazo por regla de seguridad |
| `unsupported` | acción no soportada (p. ej. `ventana/stop`) |
| `invalid_payload` | falta o sobra un campo |

**Flujo recomendado:** optimista (pintar "enviando") → esperar `ack` → refrescar
`state` → confirmar con `state.channels.<id>.on`. **El estado real es el de
`state`, no el del `ack`.**

### 5.4 Restricciones por capacidades

Antes de ofrecer un control, leer `capabilities`:

- `window_stop: false` → **no mostrar botón "detener"**
- `window_actuator: pulse_toggle_5s` → `open`/`close` son toggles; no asumir
  estado continuo
- `enabled_channels` → ocultar o deshabilitar el resto
- `state.channels.<id>.enabled: false` → el canal está apagado **en el equipo**

---

## 6. Alarmas

### 6.1 Catálogo (11 condiciones, pueden estar varias a la vez)

| Clave | Rótulo sugerido para la web | Latch | Crítica |
|---|---|---|---|
| `temp_max` | Temperatura de aire alta | no | ✅ |
| `temp_min` | Temperatura de aire baja | no | ✅ |
| `sin_conexion` | Sin conexión con el servidor | no | ❌ |
| `humedad_persistente_baja` | Humedad de sustrato baja (60 min) | no | ❌ |
| `bed_temp_high` | Cama caliente por encima del máximo | **sí** | ✅ |
| `window_fault` | Falla en la ventana | **sí** | ✅ |
| `sensor_fault_aire` | Sensor de aire fuera de servicio | no | ❌ |
| `sensor_fault_humedad_z1` | Sensor de sustrato Z1 fuera de servicio | no | ❌ |
| `sensor_fault_humedad_z2` | Sensor de sustrato Z2 fuera de servicio | no | ❌ |
| `sensor_fault_cama` | Sensor de cama fuera de servicio | no | ❌ |
| `irrigation_timeout` | Riego cortado por tiempo máximo | **sí** | ✅ |

### 6.2 Comportamiento a respetar

- **Listar, no mostrar una sola.** Pueden haber varias activas simultáneamente.
- **Distinguir `active` de `latched`.** Un latch queda aunque la condición se
  normalice; se limpia con `command alarma / reset`.
- **Silencio ≠ resuelta.** `silenced_until` significa que el relé dejó de sonar
  10 min, pero las condiciones siguen. Mostrar "silenciada hasta HH:MM".
- Una **condición crítica nueva durante el silencio cancela el silencio**.
- **Hoy solo hay `state.alarms.{active,latched,silenced_until}`** — no hay
  histórico con timestamp de cuándo se levantó/cuándo se limpió. Ver §9.

---

## 7. Eventos / auditoría

Códigos completos en `02 §5.4`. Grupos que la web casi seguro usa:

- **Cadena de agua:** `IRRIGATION_START`, `IRRIGATION_STOP`,
  `IRRIGATION_SKIPPED`, `SCHEDULE_MISSED`
- **Alarmas:** `ALARM_RAISED`, `ALARM_CLEARED`, `ALARM_SILENCED`
- **Ventana:** `WINDOW_OPEN`, `WINDOW_CLOSE`, `WINDOW_TIMEOUT`, `WINDOW_FAULT`
- **Clima:** `THERMOSTAT_ON/OFF`, `LIGHT_ON/OFF`, `FAN_ON/OFF`
- **Config:** `CONFIG_APPLIED`, `CONFIG_REJECTED`, `CONFIG_SOURCE_CHANGED`
- **Firmware:** `BOOT`, `WATCHDOG_RESET`, `NVS_ERROR`, `SENSOR_FAULT`,
  `SENSOR_RECOVERED`, `COMMAND_REJECTED`

**Requisitos:**
- Deduplicar por **`event_id`** (`<boot_id>-<seq>`), nunca por `ts`.
- Los eventos **críticos se reenvían tras un reinicio** con su `boot_id`
  original: pueden llegar repetidos en el tiempo.
- Filtrar por `code`, `severity`, `channel` y rango de fechas.
- `data` es libre pero estable por código.

---

## 8. Tiempo, unidades y convenciones

- `ts` en **UTC ISO 8601**; convertir a la zona del usuario para mostrar.
- Los **horarios de riego se configuran en hora local** (offset por defecto
  −180 = UTC-3, Argentina sin DST). Llevar `timezone_offset_min` de `state.clock`.
- **`state.clock.valid: false`** → no confiar en los horarios; avisar.
- Unidades: °C, %, s, dBm.
- Identificadores de mensaje en inglés `snake_case`; rótulos en español.

---

## 9. Huecos de API — lo que hay que pedir al backend

> **Cada hueco de esta sección ya está redactado como ticket listo para pegarse
> como issue** en [`tickets_backend.md`](tickets_backend.md): petición, respuesta
> JSON de ejemplo, criterios de aceptación y orden de implementación (T0–T8).

Prioridad descendente:

| # | Endpoint necesario | Para qué | Hoy |
|---|---|---|---|
| 1 | `GET /api/config?device=` + `PUT` | leer y escribir horarios, umbrales, clima, alarmas, canales | ❌ 404 |
| 2 | `GET /api/alarms?device=&from=&to=` | histórico de alarmas con `raised_at` / `cleared_at` | ❌ 404 |
| 3 | `GET /api/events?device=&code=&severity=&page=` | auditoría paginada y filtrable | ❌ (solo los últimos N) |
| 4 | `GET /api/commands?device=` | estado de los comandos y su `ack` | ❌ 404 |
| 5 | `GET /api/devices` | lista con estado (online, alarmas, último contacto) | ❌ (el selector va a ciegas) |
| 6 | `GET /api/diagnostics?device=` | colas pendientes, `boot_id`, último error, reconexiones | ❌ |
| 7 | `GET /api/telemetry?device=&from=&to=&metrics=` | rango arbitrario y selección de métricas | parcial vía `duration` |
| 8 | `POST /dashboard/command` → devolver el **`ack` real** | feedback inmediato a la UI | ⬜ verificar qué devuelve |

**También confirmar:**
- Retención y paginación de `series` (hoy devuelve un fijo de 240).
- Políticas de sesión (caducidad) para no perder el trabajo del operador.
- Roles/permisos: ¿puede el operador editar config o solo administrador?

---

## 10. Casos de prueba del frontend

| # | Caso | Resultado esperado |
|---|---|---|
| F1 | Métrica con `null` | muestra "sin dato", nunca `0` ni `--` |
| F2 | Equipo desconectado > 90 s | badge "desconectado" + última vez vista |
| F3 | `config_source: local` | aviso visible de override activo |
| F4 | Canal con `source: manual_local` | "control local"; el botón ON no genera error |
| F5 | Comando con `duration_s` faltante (riego) | validación en UI, no llega al backend |
| F6 | Horario con los 7 días desmarcados | queda desactivado, no "todos los días" |
| F7 | `ack: expired` | mensaje claro, sin reenviar |
| F8 | `ack: duplicate` | muestra resultado original, no duplica acción |
| F9 | Alarma latched | badge distinto de "activa"; reset limpia |
| F10 | Alarma silenciada | "silenciada hasta HH:MM", no "resuelta" |
| F11 | `capabilities.window_stop === false` | sin botón "detener" |
| F12 | 3 alarmas simultáneas | lista las 3, no una sola |
| F13 | Telemetría con `ts` viejo | marcada como "en buffer", no como en vivo |
| F14 | `window_state: open` con `window_limits: 1` | no afirma "posición abierta conocida" |
| F15 | Sesión expirada a mitad de edición | no pierde lo escrito sin avisar |

---

## 11. Referencias

| Qué | Dónde |
|---|---|
| Contrato WebSocket (mensajes, JSON) | [`../02_protocolo_ws.md`](../02_protocolo_ws.md) |
| Reglas de control, límites, precedencia | [`../03_reglas_y_plan.md`](../03_reglas_y_plan.md) |
| Alcance de v0.1 | [`../00_vision_y_alcance.md`](../00_vision_y_alcance.md) |
| Requerimientos del backend | [`backend.md`](backend.md) |
| Backend falso (ver mensajes reales) | [`../../tools/fake_backend.py`](../../tools/fake_backend.py) |
| Portal del dispositivo (referencia de campos) | `src/config_portal.cpp` |
