# Requerimientos — Frontend

**Alcance:** aplicación web que consume la API del backend para monitorear y
controlar el vivero.

> **No es trabajo tuyo** el portal cautivo del dispositivo. Ese HTML lo sirve
> el propio ESP32 en `http://192.168.4.1` y ya está implementado en
> `src/config_portal.cpp`. Sirve como **referencia de campos y validaciones**
> (qué se puede configurar), pero no se reutiliza ni se conecta a la API.

---

## 1. Qué hay disponible

### 1.1 Telemetría (cada 30 s por defecto)

| Campo | Unidad | Notas |
|---|---|---|
| `air_temp_c` | °C | aire |
| `air_rh_pct` | % | humedad relativa |
| `substrate1_moisture_pct` | % | sustrato Z1 |
| `substrate2_moisture_pct` | % | sustrato Z2 |
| `substrate1_temp_c` | °C | temperatura de sustrato Z1 |
| `substrate2_temp_c` | °C | temperatura de sustrato Z2 |
| `bed_temp_c` | °C | cama caliente |
| `window_state` | enum | `closed`, `open`, `moving`, `fault`, `unknown` |
| `rssi_dbm` | dBm | calidad de WiFi |
| `uptime_s` | s | tiempo encendido |

Cada métrica puede venir con `valid: false` (sensor caído). **La UI debe
mostrar "sin dato" y no `0`.** Ese es el caso de uso más fácil de dejar mal.

### 1.2 Estado (`state`)

- `mode`: `NORMAL` | `PORTAL` | `MANTENIMIENTO`
- `config_source`: `backend` | `local` (si el operador forzó el perfil local)
- `window_state`, alarmas activas y latched, canales habilitados, versión de
  config, uptime, RSSI.

### 1.3 Eventos

Auditoría completa con `code`, `severity`, `channel`, `message`, `data` y
`ts`. Lista completa en [`../02_protocolo_ws.md`](../02_protocolo_ws.md) §5.4.

---

## 2. Pantallas que hay que poder hacer

Esto es lo que los datos permiten mostrar; el diseño es tuyo.

| Pantalla | Contenido mínimo |
|---|---|
| **Listado de dispositivos** | nombre, conectado/desconectado, modo, última actividad, alarmas activas |
| **Detalle** | estado actual, las 7 variables en vivo + gráfico de las últimas 24 h/7 d, RSSI, uptime, versión de firmware |
| **Alarmas** | activas, latched (con badge distinto), historial con `ts`, valor y `data.value` |
| **Riego** | estado por zona (Z1, Z2, aspersión), horarios, umbral, duración, `max_run_s`; botón de riego manual con duración |
| **Historial de riego** | eventos `IRRIGATION_START/STOP/SKIPPED` con motivo (`schedule`, `threshold`, `manual_local`, `manual_remote`, `max_run`, `interlock`…) |
| **Configuración** | edición de la config versionada (ver §4) |
| **Comandos** | enviar `on/off`, abrir/cerrar ventana, silenciar alarma, con feedback del `ack` |

---

## 3. Comandos que puede enviar la web

| Canal | Acciones | Notas para la UI |
|---|---|---|
| `riego_z1`, `riego_z2`, `aspersion` | `on` / `off` | `on` **pide duración** (1 s … `max_run_s`) |
| `iluminacion`, `ventilacion` | `on` / `off` | duración opcional (default 600 s) |
| `cama_caliente` | `on` / `off` / `auto` | `auto` devuelve al termostato; `on` tiene corte duro a 35 °C |
| `ventana` | `open` / `close` | **es un pulso de 5 s**. No hay botón "stop": `stop` responde `unsupported` |
| `alarma` | `silence` / `reset` | `silence` silencia 10 min; `reset` limpia los latches |

### Estados del `ack` que la UI debe saber interpretar

| `status` | Qué mostrar |
|---|---|
| `ok` | acción aceptada (y `on`/`until` con el resultado) |
| `expired` | el comando llegó tarde: "el comando venció, reintentá" |
| `duplicate` | era un reintento; muestra el resultado original, **no reenvíes** |
| `local_manual_active` | **"Alguien en el equipo tiene el control manual"** — el `off` sí se acepta siempre |
| `channel_disabled` | "el canal está deshabilitado en el equipo" |
| `interlock` | "hay otro riego en curso" |
| `safety_interlock` | el equipo lo rechazó por una regla de seguridad |
| `unsupported` | acción no soportada (p. ej. `ventana/stop`) |
| `invalid_payload` | falta o sobra un campo |

**Regla de UX importante:** el equipo puede estar en `MANTENIMIENTO`, donde
todos los relés están OFF y la automatización suspendida. En ese modo los
comandos no van a hacer nada útil. **Mostrar el modo en toda vista de control.**

---

## 4. Configuración editable

La config viaja versionada en el mensaje `config` (estructura completa en
`02 §6.2`). Lo que la web va a querer editar:

- **WiFi** del dispositivo (SSID/contraseña)
- **URL del backend** y URL WebSocket
- **Fuente de configuración**: `backend` (default) o `local` (override)
- **Habilitación de los 8 canales**
- **Horarios** por zona: inicio, duración, **días de la semana** (7 bits,
  lunes = bit 0)
- **Umbrales**: humedad mínima por zona, habilitación del riego por umbral
- **Interlocks y límites**: `interlock`, `max_run_s`, `min_off_s`, `stagger_s`
- **Cama caliente**: setpoint y flag de automático
- **Ventilación**: automático, umbrales de temperatura y humedad
- **Alarmas**: temperatura máxima/mínima
- **Zona horaria**: offset en minutos (default `-180` = UTC-3)
- **Clave del AP**

Validaciones que el firmware aplica (y que la web debería replicar para no
mandar algo que se va a rechazar):

- Duraciones 1…3600 s; `min_off_s` ≤ 43200; `stagger_s` ≤ 600.
- Umbral de humedad 0…100 %.
- Setpoint de cama 5…34.9 °C (por encima está el corte duro de 35 °C).
- Clave del AP mínimo 8 caracteres.
- Un horario con **los 7 días desmarcados queda desactivado**.
- El backend puede mandar lo que quiera, pero el dispositivo **valida todo y
  responde `CONFIG_REJECTED` con `data.errors`** sin aplicar cambios parciales.

---

## 5. Catálogo de alarmas (para mostrar)

Las 11 condiciones del firmware. **Pueden estar activas varias a la vez**:
la UI debe listar, no mostrar una sola.

| Clave | Rótulo OLED (≤14 car.) | Rótulo sugerido para la web | Latch |
|---|---|---|---|
| `temp_max` | ALARMA TEMP MAX | Temperatura de aire alta | no |
| `temp_min` | ALARMA TEMP MIN | Temperatura de aire baja | no |
| `sin_conexion` | ALARMA SIN CONEX | Sin conexión con el servidor | no |
| `humedad_persistente_baja` | ALARMA HUM. BAJA | Humedad de sustrato baja (60 min) | no |
| `bed_temp_high` | ALARMA CAMA MAX | Cama caliente por encima del máximo | **sí** |
| `window_fault` | ALARMA VENTANA | Falla en la ventana | **sí** |
| `sensor_fault_aire` | FALLA SENSOR AIRE | Sensor de aire fuera de servicio | no |
| `sensor_fault_humedad_z1` | FALLA HUM Z1 | Sensor de sustrato Z1 fuera de servicio | no |
| `sensor_fault_humedad_z2` | FALLA HUM Z2 | Sensor de sustrato Z2 fuera de servicio | no |
| `sensor_fault_cama` | FALLA SENSOR CAMA | Sensor de cama fuera de servicio | no |
| `irrigation_timeout` | ALARMA RIEGO | Riego cortado por tiempo máximo | **sí** |

- **Latch** = queda pegada hasta que alguien la resetee (`command alarma
  reset`). Distinguish claramente "activa" de "latched": una condición puede
  haberse normalizado y el latch seguir puesto.
- **Silencio** = el relé dejó de sonar 10 min, pero las condiciones siguen.
  Mostrar "silenciada hasta HH:MM" y no "resuelta".

---

## 6. Canales

| Nombre de protocolo | Rótulo |
|---|---|
| `riego_z1` | Riego Z1 |
| `riego_z2` | Riego Z2 |
| `aspersion` | Aspersión |
| `cama_caliente` | Cama caliente |
| `iluminacion` | Iluminación |
| `ventilacion` | Ventilación |
| `ventana` | Ventana |
| `alarma` | Alarma |

Los canales son **fijos en firmware**; solo se pueden habilitar o
deshabilitar. La UI no debe ofrecer "reasignar GPIO" ni "cambiar la función".

---

## 7. Convenciones

- **Idioma:** español. Claves del protocolo en inglés snake_case.
- **Hora:** `ts` llega en **UTC ISO 8601**; convertir a la zona del usuario
  para mostrar. Los horarios de riego se configuran en **hora local**
  (offset por defecto UTC-3, Argentina sin horario de verano).
- **Unidades:** °C, %, s, dBm.
- **Valores inválidos:** `valid: false` → "sin dato", nunca `0`.
- **Modo `PORTAL`:** el dispositivo está configurándose; la automatización
  sigue activa (no es una alarma).
- **Modo `MANTENIMIENTO`:** relés OFF a propósito. No mostrarlo como falla.

---

## 8. Referencias

| Qué | Dónde |
|---|---|
| Contrato completo (mensajes, ejemplos JSON) | [`../02_protocolo_ws.md`](../02_protocolo_ws.md) |
| Reglas de control y límites | [`../03_reglas_y_plan.md`](../03_reglas_y_plan.md) |
| Alcance de v0.1 | [`../00_vision_y_alcance.md`](../00_vision_y_alcance.md) |
| Backend falso (para ver mensajes reales) | [`../../tools/fake_backend.py`](../../tools/fake_backend.py) |
| Portal del dispositivo (referencia de campos) | `src/config_portal.cpp` |
