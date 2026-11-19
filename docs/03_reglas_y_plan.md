# Vivero IoT — Reglas de operación y plan (v0.1)

**Fecha:** 16/11/2026
**Estado:** M0–M6 implementados; M6 en validación de banco. Depende de las decisiones abiertas de `00_vision_y_alcance.md`.
**Relacionado:** `01_hardware_pinout.md`, `02_protocolo_ws.md`.

## 1. Principios de control

1. **La seguridad manda sobre todo.** Los límites de tiempo, temperatura máxima,
   interlocks y sensores inválidos no se pueden anular desde la web ni desde los
   botones.
2. **El sitio manda sobre lo remoto.** Una orden manual local tiene prioridad
   sobre una orden web mientras esté activa.
3. **Lo remoto manda sobre la automatización.** Una orden web con vencimiento
   reemplaza al horario o al umbral para ese canal.
4. **La automatización manda sobre el reposo.** Si no hay orden ni condición, el
   canal queda OFF.
5. **Sin backend, el equipo sigue vivo.** Horarios, umbrales, termostato y
   botones usan la última configuración válida guardada en NVS.
6. **La habilitación es local.** Un canal deshabilitado en el portal no se
   automatiza, no se comanda y no aparece en el menú de botones.
7. **Todo arranca apagado.** Ningún relé queda energizado por un reinicio, un
   corte o un watchdog.

### 1.1 Orden de precedencia

```
Seguridad (cortes, máximos, interlocks, sensor inválido)
  > Manual local (botones, con timeout)
    > Manual remoto (command con expires_at)
      > Automatización (horario, umbral, termostato, clima)
        > OFF
```

La alarma es independiente: su relé no participa de esta precedencia y se
gobierna por el motor de alarmas (§10).

### 1.2 Fuentes de mando

| Fuente | Quién | Vigencia | Se muestra como |
|---|---|---|---|
| `manual_local` | botones Selector/Confirmar | timeout local (default por canal) | `manual_local` |
| `manual_remote` | web/backend | `expires_at` + duración | `manual_remote` |
| `auto` | firmware | mientras la condición se mantenga | `auto` |
| `alarm` | motor de alarmas | mientras la regla esté activa | `alarm` |
| `test` | portal cautivo | 5–30 s, solo en mantenimiento | `test` |

## 2. Modos de operación

| Modo | WiFi | Automatización | Botones | Portal | WebSocket |
|---|---|---|---|---|---|
| `NORMAL` | STA | sí | sí | solo al entrar (corta STA) | sí |
| `PORTAL` | AP | sí, con config cacheada | sí | sí | no (se corta) |
| `MANTENIMIENTO` | AP | no; relés OFF | no (solo portal) | sí | no |

### 2.1 Entradas y salidas

- **Arranque:** si no hay credenciales WiFi → `PORTAL`. Si hay → `NORMAL`.
- **Botón Configuración (GPIO39):** pulsación corta (< 2 s) → `PORTAL`.
  Pulsación larga (≥ 5 s) → `PORTAL` + opción de borrar solo WiFi (no borra
  configuración ni calibración) con confirmación en OLED.
- **Falla de WiFi > 60 s:** → `PORTAL` para diagnóstico; al salir, reintenta STA.
- **Guardar WiFi en el portal:** vuelve a `NORMAL` sin reiniciar.
- **Timeout de inactividad del portal:** 15 min → `NORMAL` sin reiniciar.
- **Mantenimiento:** se activa solo desde el portal con confirmación explícita.
  Al salir vuelve a `NORMAL` y reevalúa la automatización desde cero.
- **Reinicio:** nunca reiniciar para aplicar configuración. El único reinicio
  automático es por watchdog o por actualización de firmware.
- **AP:** `Vivero-IoT-<id>` con WPA2. Clave inicial derivada de la MAC, visible en
  el OLED; se puede cambiar desde el portal.

### 2.2 Qué sigue funcionando en cada modo

- **`PORTAL`:** la automatización continúa con la última config. Los horarios se
  evalúan con el RTC. Los botones siguen activos. Los eventos y la telemetría se
  bufferizan para enviar al reconectar.
- **`MANTENIMIENTO`:** todos los relés OFF (incluida la alarma), automatización
  detenida, botones ignorados. El portal puede probar un canal por vez con
  auto-off a los 30 s (en ventana, un pulso de 5 s). Es el único modo donde se
  suspende el control.
- En cada transición de modo se descartan las pulsaciones pendientes
  (`buttonsClearPending()`). En `PORTAL` y `MANTENIMIENTO` los botones no se
  atienden; sin eso, una pulsación "ignorada" se ejecutaría al volver a
  `NORMAL` (p. ej. reabrir el portal justo al salir de él).
- Si se entra a mantenimiento con una cama caliente encendida, se apaga y se
  registra el evento. Al salir, el termostato decide de nuevo.
- Si se entra con la ventana en movimiento, el pulso se aborta y el estado se
  resincroniza con el sensor (`windowAbortMotion()`). Sin esto, `relayAllOff()`
  cortaría el pulso en seco y `WINDOW_FAULT` (latch por defecto) se levantaría
  20 s después por algo que hizo el propio firmware. Un `FAULT` anterior sí se
  conserva: es una falla real del sensor.
- Si se entra con un riego activo, se detiene y se cierra su evento
  (`irrigationStopAllActive()`), **sin** disparar `irrigation_timeout`: el corte
  lo pide el modo, no el tiempo de riego. Al salir, la automatización vuelve a
  evaluar y puede reanudar si el horario o el umbral sigue cumpliéndose.

### 2.3 Portal cautivo

- AP `Vivero-IoT-<id>` con WPA2. DNS cautivo para que el teléfono abra la página.
- El portal permite: WiFi (SSID/password), clave del AP, URL del backend,
  `claim_code`, zona horaria, habilitar/deshabilitar los 8 canales, probar canales,
  calibrar humedad, ver las 3 sondas DS18B20, editar el **perfil local** de
  horarios/umbrales, elegir la fuente de configuración (`backend` o `local`),
  ver estado/versión y activar mantenimiento.
- **No** permite cambiar la función ni el GPIO de un canal.
- **Días de un horario:** desmarcar los 7 días desactiva el horario. El
  formulario envía un campo oculto `daym_*` para distinguir "sin días" de "el
  formulario llegó incompleto"; en ese segundo caso se sigue asumiendo "todos
  los días" por seguridad. Antes los dos casos se convertían en "todos los
  días", que era una inversión del sentido.
- **Límite de la OLED:** 128 px con fuente de 6 px → 21 caracteres por línea.
  SSID y clave se cortan a los 18 caracteres útiles y se marcan con "..." (antes
  el corte era silencioso). La clave derivada de la MAC (12 caracteres) siempre
  entra completa; el propio portal avisa del límite al cargar una clave larga.
- **Configuración activa:** por defecto manda la config del backend. Si no hay
  ninguna, manda el perfil local. El operador puede activar **override local**;
  mientras esté activo, el backend sigue enviando config pero el equipo la guarda
  sin aplicarla, y la web lo ve en `state.config_source`.
- **Edición local:** el portal valida los mismos rangos que el backend. Guardar el
  perfil local incrementa `local_version` y aplica de inmediato solo si la fuente
  activa es `local`. Si la fuente es `backend`, el cambio queda como respaldo y la
  UI lo indica.
- Al volver de `local` a `backend`, se aplica la última config de backend
  almacenada (si hay) y se emite `CONFIG_SOURCE_CHANGED`.

## 3. Control local con botones

### 3.1 Navegación

- **Selector (GPIO25):** avanza al siguiente canal habilitado. El orden es fijo:
  `riego_z1`, `riego_z2`, `aspersion`, `cama_caliente`, `iluminacion`,
  `ventilacion`, `ventana`. La alarma no está en la lista.
- **Confirmar (GPIO36):** ejecuta la acción del canal mostrado.
- El OLED muestra: canal, estado actual (`ON`/`OFF`/`ABRIENDO`/`CERRANDO`),
  origen (`LOCAL`, `WEB`, `AUTO`), tiempo restante y alarmas activas.
- Si no hay canales habilitados, el OLED muestra “Sin canales habilitados”.
- Los botones tienen debounce de 30 ms y repetición solo para el Selector
  (500 ms) si se mantiene presionado.

### 3.2 Acción de Confirmar por canal

| Canal | Acción |
|---|---|
| `riego_z1`, `riego_z2` | alterna ON/OFF; ON con 600 s |
| `aspersion` | alterna ON/OFF; ON con 300 s |
| `cama_caliente` | alterna ON/OFF; ON manual con 1800 s y corte por `max_c` |
| `iluminacion` | alterna ON/OFF; ON con 3600 s |
| `ventilacion` | alterna ON/OFF; ON con 1800 s |
| `ventana` | CLOSED → abrir; OPEN → cerrar; MOVING/FAULT → ignorar |

- Una orden local reemplaza la remota y la automática del canal hasta su timeout
  o hasta que el operador la apague.
- El timeout se muestra en el OLED y se puede cancelar con Confirmar.
- En ventana no hay parada: si está `MOVING` o `FAULT`, Confirmar no hace nada.
  La recuperación de `FAULT` se hace desde el portal en mantenimiento.
- Si el canal está deshabilitado, no aparece; si se deshabilita mientras está
  encendido, se apaga de inmediato y se registra el evento.
- **Silenciar alarma sin web:** con una alarma activa, mantener Confirmar 3 s
  silencia el relé 10 min (no borra latches). El OLED lo confirma.

### 3.3 Interacción con la automatización

- Mientras hay una orden local activa, la automatización no toca ese canal.
- Al terminar el timeout, el canal vuelve a `auto` si la condición sigue vigente;
  si no, queda OFF.
- Un riego local no actualiza el temporizador de `min_off` del riego por umbral
  como si fuera un ciclo automático; solo pospone el próximo ciclo.
- Los botones están activos en `NORMAL` y `PORTAL`, no en `MANTENIMIENTO`.

## 4. Riego (Z1, Z2 y aspersión)

### 4.1 Fuentes de encendido

1. **Horario:** una entrada `start` + `duration_s` en días seleccionados.
2. **Umbral de humedad (solo Z1/Z2):** si `moisture.enabled` y la humedad cae
   por debajo de `start_below_pct` durante 60 s, dentro de la ventana horaria.
3. **Manual local o remoto:** duración explícita.

### 4.2 Reglas

- **Un solo riego a la vez:** si `irrigation.interlock` es `true`, no pueden estar
  activos simultáneamente `riego_z1`, `riego_z2` y `aspersion`. Al terminar uno,
  se espera `stagger_s` antes de habilitar el siguiente.
- **Duración máxima:** `duration_s` se recorta a `max_run_s` del canal. Al corte
  se emite `IRRIGATION_STOP` con `reason: "max_run"` y se activa
  `irrigation_timeout` (posible alarma).
- **Cadencia del corte:** la duración y `max_run_s` se evalúan en **cada
  pasada del loop** (`irrigationEnforceLimits()`), no solo en el ciclo local
  de 30 s. El corte es así exacto y una zona no puede quedar encendida aunque
  el ciclo local no corra. El ciclo local sigue decidiendo si *arranca* un
  riego.
- **Tiempo mínimo apagado:** `min_off_s` se aplica entre ciclos automáticos por
  umbral. Un riego manual (local o remoto) puede arrancar sin esperarlo, porque
  el operador está presente; al terminar, el próximo ciclo automático sí espera
  `min_off_s` desde ese apagado.
- **Ventana horaria del umbral:** `window_start`/`window_end` en hora local. Si
  `window_start > window_end`, se interpreta como ventana que cruza medianoche.
- **Histéresis:** arranca por debajo de `start_below_pct`; corta al alcanzar
  `stop_above_pct`. Si `stop_above_pct <= start_below_pct`, la config es inválida.
- **Confirmación:** la lectura debe sostenerse 60 s antes de arrancar, para evitar
  riegos por una lectura espuria. La parada por umbral es inmediata.
- **Horario:** una entrada se dispara una vez por día y por `entry_id`. La clave de
  deduplicación incluye la fuente (`backend:` o `local:`) para que los perfiles no
  choquen. Si el equipo arranca dentro de los 60 s posteriores al `start`, se
  ejecuta; pasado ese margen se omite (`SCHEDULE_MISSED`). No hay riego
  retroactivo.
- **Solapamiento de horarios:** si dos entradas del mismo canal se solapan, gana
  la primera y la segunda se omite con evento.
- **Humedad inválida:** el riego por umbral de esa zona se deshabilita; el
  horario y el manual siguen funcionando. Se activa `sensor_fault`.
- **Sin RTC válido:** no se ejecutan horarios; el umbral y el manual sí.
- **Aspersión:** solo horario y manual, sin umbral de humedad. `max_run_s` igual
  que las otras zonas.
- **Falla de riego:** en v0.1 no hay caudalímetro. Se considera falla el corte
  por `max_run` y la imposibilidad de arrancar por interlock repetido; no se
  infiere de la humedad.

### 4.3 Ejemplo de ciclo por umbral

```
Humedad Z1 = 28 % (< 30 %) durante 60 s, dentro de 06:00–20:00
→ ON, source=auto, until=now+max_run_s
Humedad Z1 = 45 % (>= stop_above)
→ OFF, event IRRIGATION_STOP reason=done
→ min_off_s = 1800 s antes del próximo ciclo
```

## 5. Cama caliente (termostato)

- **Sensor:** `bed_temp_c` (DS18B20 de la cama). Si es inválido, el relé va a OFF
  y se activa `sensor_fault`. No se usa la temperatura del aire como respaldo.
- **Banda:** ON cuando `T <= setpoint_c - hysteresis_c`; OFF cuando
  `T >= setpoint_c`. La banda efectiva es `[setpoint - h, setpoint]`.
- **Corte absoluto:** si `T >= max_c`, OFF inmediato y alarma `bed_temp_high`.
  Este corte no se puede anular ni con manual local ni remoto.
- **Anti-ciclado:** mínimo 60 s encendido y 120 s apagado entre conmutaciones
  (constantes de firmware v0.1).
- **Manual:** `on` fuerza encendido con timeout de 1800 s, siempre sujeto a
  `max_c` y a sensor válido; `auto` vuelve al termostato; `off` fuerza apagado.
- **Sin conexión:** el termostato sigue funcionando con la última config.
- **Sin RTC:** el termostato funciona igual (no depende de horarios).

## 6. Iluminación

- **Horario:** `start` + `duration_s` en días seleccionados. Es la única fuente
  automática en v0.1.
- **Manual:** local 3600 s o remoto con `duration_s` (máximo `max_run_s`).
- **Seguridad:** corte duro a las 16 h continuas, aunque la config pida más; se
  registra evento y se apaga. Esto protege el cultivo y la instalación.
- **Solapamiento:** si dos entradas se solapan, gana la primera; la segunda se
  omite con evento.
- **Sin RTC:** no hay automatización; el manual sigue disponible.

## 7. Ventilación forzada

- **Clima:** ON si `temp >= on_above_temp_c` **o** `RH >= on_above_rh_pct`,
  sostenido 60 s. OFF si `temp <= off_below_temp_c` **y**
  `RH <= off_below_rh_pct`, sostenido 60 s. La banda evita oscilación.
- **Horario:** las entradas de `schedule` son un pedido de ON adicional. El
  ventilador queda ON si el horario lo pide **o** si el clima lo pide; solo pasa
  a OFF cuando ambos dejan de pedirlo.
- **Tiempos mínimos:** `min_on_s` y `min_off_s` se respetan salvo corte por
  seguridad o por `off` manual/remoto.
- **Máximo:** `max_run_s` corta el ventilador y lo deja en `min_off_s`; se emite
  evento. Si la condición climática persiste, vuelve a arrancar tras el descanso.
- **Sensor de aire inválido:** el clima se deshabilita; el horario y el manual
  siguen. Se activa `sensor_fault`.
- **Manual:** local 1800 s; remoto con duración. Preempta clima y horario.
- **Sin RTC:** el clima y el manual funcionan; el horario no.

## 8. Ventana (controlador toggle por pulso)

**Hardware confirmado:** el relé 7 no alimenta el motor. Entrega un **pulso de 5 s**
a un controlador que **alterna** el sentido (un pulso abre, el siguiente cierra).
El controlador maneja los límites internos. El ESP32 solo tiene un **sensor de
cerrada** en GPIO13; no hay confirmación de apertura total ni entrada de parada.
El segundo sensor se evalúa para v2.

### 8.1 Estados

| Estado | Condición | Significado |
|---|---|---|
| `CLOSED` | sensor de cerrada activo | cerrada y confirmada |
| `OPEN` | sensor inactivo, sin movimiento y `travel_time_s` cumplido | abierta (deducida) |
| `MOVING` | pulso activo o esperando transición/tiempo | en tránsito; `direction` indica apertura/cierre |
| `FAULT` | el sensor no cambió dentro de `max_travel_s` | controlador o sensor trabado |
| `UNKNOWN` | arranque, antes de la primera lectura | transitorio |

- Debounce de 50 ms. Cerrada = LOW.
- Al arrancar: si el sensor está activo → `CLOSED`; si no → `OPEN`. No se puede
  distinguir “abierta” de “entreabierta”; es la limitación de v1.
- Un sensor trabado en “no cerrada” puede hacer que un cierre genere una apertura.
  Mitigación: probar el sensor en el portal antes de habilitar la automatización;
  si no cambia tras un pulso, `FAULT` y no reintentar automáticamente.
- **v2:** agregar sensor de abierta para confirmar ambos extremos y detectar el
  sensor trabado. Requiere GPIO15 boot-safe o un expansor I2C.

### 8.2 Movimiento

- **Abrir:** solo desde `CLOSED`. Energizar el relé `pulse_ms` (5000 ms), soltarlo,
  pasar a `MOVING/opening`. El sensor debe liberarse dentro de `max_travel_s`; si
  no, `FAULT`. Con el sensor liberado y `travel_time_s` cumplido desde el pulso,
  declarar `OPEN`.
- **Cerrar:** solo desde `OPEN`. Pulso de 5 s, `MOVING/closing`. El sensor debe
  activarse dentro de `max_travel_s`; si no, `FAULT`.
- Si ya está en el estado pedido: `ack ok` con `applied.noop: true`, sin pulso.
- En `MOVING`: rechazar con `busy`; nunca un segundo pulso.
- En `FAULT`: rechazar. La recuperación es solo desde el portal en mantenimiento,
  con un pulso temporizado y el operador mirando la ventana. No hay reintento
  automático.
- **Sin `stop`:** el controlador toggle no tiene parada. Un pulso extra invertiría
  el sentido. El comando `stop` se rechaza como `unsupported`.
- `min_interval_s` (default 60 s) entre pulsos. Si no se cumplió, `busy`.
- El relé nunca queda energizado: el pulso se maneja con temporizador no bloqueante
  y un guard de 6 s. Si el firmware se cuelga, el watchdog reinicia y los pull-ups
  dejan el relé OFF.
- Entre la orden y el pulso, verificar que el estado siga siendo el esperado; si
  cambió, abortar.

### 8.3 Automatización

- **Abrir** si `temp >= open_above_temp_c` **o** `RH >= open_above_rh_pct`,
  sostenido 120 s, con estado `CLOSED` y `min_interval_s` cumplido.
- **Cerrar** si `temp <= close_below_temp_c` **y** `RH <= close_below_rh_pct`,
  sostenido 300 s, con estado `OPEN`.
- No se mueve si el sensor de aire es inválido.
- No hay sensor de lluvia ni de viento en v1. Si se agrega, el cierre por lluvia
  tendrá prioridad sobre la temperatura.
- El manual local/remoto preempta la automatización. Tras un manual, la
  automatización espera 5 min antes de volver a mover.
- Si `auto_enabled` es `false`, no hay movimientos automáticos; el manual sigue.

### 8.4 Control local y remoto

- **Confirmar:** `CLOSED` → abrir; `OPEN` → cerrar; `MOVING` → ignorar; `FAULT` →
  ignorar (solo portal).
- **Remoto:** `open`/`close` con la misma deducción de sentido. `stop` →
  `unsupported`.
- El ack informa `applied.noop`, `state` y `direction` cuando corresponde.

## 9. Alarmas

- Reglas configurables por el backend (ver `02_protocolo_ws.md` §7): métrica,
  operador, umbral, histéresis, retardo, severidad y latch.
- **Múltiples condiciones simultáneas:** `alarms.active` es un array. El relé de
  alarma se energiza si hay al menos una regla activa y no está silenciado.
- **Latch:** una regla con `latch: true` queda activa hasta `reset`; si la
  condición sigue, se reactiva al limpiar.
- **Silencio:** apaga el relé por `duration_s` sin borrar condiciones. Una alarma
  `critical` nueva durante el silencio vuelve a energizar.
- **Persistencia:** los latches se guardan en NVS y sobreviven un reinicio. Las
  condiciones se reevalúan al arrancar; si persisten, la alarma vuelve a sonar.
- **Alarma y mantenimiento:** en `MANTENIMIENTO` el relé de alarma queda OFF por
  seguridad del técnico, pero las condiciones se siguen evaluando y registrando.
- **Canal de alarma deshabilitado:** se permite, pero el portal debe mostrar una
  advertencia roja. Las reglas siguen activas y los eventos se registran; no hay
  salida física.
- **Sin conexión:** las reglas se evalúan localmente; `offline_s` puede disparar
  la alarma de conexión. Los eventos se encolan.
- **Reset local:** mantener Confirmar 3 s silencia 10 min; el reset de latches se
  hace desde el portal o la web.

## 10. Fail-safe, límites y sensores inválidos

### 10.1 Límites duros

| Canal | Límite | Al superarlo |
|---|---|---|
| `riego_z1`, `riego_z2` | `max_run_s` (default 900 s), tope absoluto 3600 s | OFF + `IRRIGATION_STOP max_run` |
| `aspersion` | `max_run_s` (default 1800 s), tope absoluto 3600 s | OFF + evento/alarma |
| `iluminacion` | 16 h continuas | OFF + evento, aunque la config pida más |
| `ventilacion` | `max_run_s` (default 3600 s) | OFF + `min_off_s` de descanso |
| `cama_caliente` | `max_c` y sensor válido | OFF inmediato + alarma |
| `ventana` | `pulse_ms`, `max_travel_s`, `min_interval_s` | soltar relé + `FAULT` + alarma |
| Todos | watchdog, brownout, mantenimiento | OFF |

- Los topes absolutos no son configurables por el backend; están en firmware.
- `max_run_s` se valida en la config: si viene mayor que el tope, se recorta y se
  informa en `warnings`.

### 10.2 Sensores inválidos

| Falla | Efecto inmediato | Automatización afectada |
|---|---|---|
| DHT21 (aire) | `sensor_fault` | se deshabilitan clima de ventilación y automatización de ventana |
| Humedad Z1/Z2 | `sensor_fault` | se deshabilita el umbral de esa zona; horario y manual siguen |
| DS18B20 de cama | `sensor_fault` | cama OFF; no se puede encender hasta recuperar |
| DS18B20 Z1/Z2 | `sensor_fault` | solo se pierde la telemetría de esa sonda; no bloquea riego |
| Sensor de ventana (cerrada) | `WINDOW_TIMEOUT`/`FAULT` | se rechazan movimientos hasta diagnóstico |
| RTC | `TIME_INVALID` | no hay horarios; umbral, termostato y manual siguen |
| OLED/I2C | evento `DISPLAY_FAULT` | no afecta el control |
| WiFi/WebSocket | `WS_LOST` | nada; solo buffer y alarma `offline` si está configurada |

- Un sensor se declara inválido tras 3 lecturas fallidas consecutivas; se
  recupera tras 3 lecturas válidas consecutivas.
- Un valor `null` nunca se interpreta como 0.
- La cama caliente es la única función que se apaga por pérdida de su sensor.

### 10.3 Secuencia de arranque

1. Antes de `pinMode`, escribir HIGH en los 8 GPIO de relé (low-level = OFF).
2. Configurar los pines como salida y mantener OFF.
3. Leer NVS. Si la config está corrupta o ausente, cargar defaults seguros:
   automatización OFF, horarios vacíos, cama OFF, alarmas básicas de sensor.
4. Inicializar I2C, OLED, RTC, sensores. Esperar 2 s de estabilización.
5. Determinar modo (`NORMAL` o `PORTAL`) según credenciales.
6. Evaluar seguridad (sensores, límites), luego automatización.
7. Emitir `BOOT` con `reset_reason` y `boot_count`.

- Ningún relé debe pulsar durante el arranque. Verificar con carga desconectada.
- El watchdog de tarea se alimenta en el loop principal; si se cuelga, el reset
  deja los relés OFF por los pull-ups externos.

## 11. Tiempo, horarios y persistencia

### 11.1 Reloj

- DS3231 por I2C. Si la hora es válida, los horarios funcionan sin WiFi.
- NTP cuando hay STA; `server_time` del backend tiene prioridad.
- Sin hora válida: horarios OFF, umbral/termostato/manual ON, comandos remotos
  restringidos (§6.3 del protocolo).
- Los horarios se evalúan en hora local (`UTC + timezone_offset_min`). No hay
  cambio de horario automático en v0.1.
- Cada entrada guarda su último disparo (`source:entry_id` + fecha UTC) en NVS
  para no repetir el riego si el equipo se reinicia dentro del mismo día.

### 11.2 NVS

| Namespace / clave | Contenido | Cuándo se escribe |
|---|---|---|
| `cfg/wifi_ssid`, `cfg/wifi_pass` | credenciales STA | al guardar el portal |
| `cfg/ap_pass` | clave del AP (vacío = derivada de MAC) | al guardar el portal |
| `cfg/claim_code` | código de alta del dispositivo | al guardar el portal |
| `cfg/device_id`, `cfg/api_token` | identidad provisionada | al reclamar/rotar |
| `cfg/backend_url` | URL base del backend | al guardar el portal |
| `cfg/ws_url` | URL WebSocket devuelta por el claim o cargada a mano | al reclamar/guardar |
| `cfg/tz_off` | offset local en minutos (UTC-3 = -180) | al guardar portal/config |
| `cfg/ch_en` | bitmask de 8 canales | al cambiar en el portal |
| `cfg/bed_target` | setpoint de cama | al guardar el portal |
| `cfg/backend_prof`, `cfg/cfg_version` | última config del backend | al recibir/aplicar `config` |
| `cfg/local_prof`, `cfg/local_ver` | perfil local del portal | al guardar horarios/umbrales |
| `cfg/cfg_source` | `backend` o `local` | al cambiar el override |
| `cfg/alarm_latch` | latches de alarma | al cambiar |
| `calib/dryN`, `calib/wetN`, `calib/calN` | calibración de humedad | al calibrar |
| `sched/last_run` | último disparo por `source:entry_id` | al disparar |
| `ws_crit/ring` | ring de 8 eventos críticos | al ocurrir/confirmar envío |
| `stats/boot_count`, `stats/reset_reason` | diagnóstico | al arrancar |

- No se escribe NVS en cada lectura ni en cada telemetría.
- Los cambios de config se agrupan y se escriben una vez, con verificación de
  lectura posterior.
- Si falla la escritura, se conserva la config anterior y se emite `NVS_ERROR`.

### 11.3 Reset de fábrica

Dos caminos, ambos terminan en `PORTAL` (el equipo reinicia sin WiFi):

1. **Gesto físico al arrancar:** mantener Configuración 10 s desde el encendido.
   La OLED muestra la cuenta regresiva; al soltar, pide pulsar Confirmar dentro
   de 5 s. Si no se confirma, el arranque continúa normal.
   El gesto está acotado y diagnóstico porque GPIO36/39 no tienen pull-up
   interno: la espera de soltura se corta a los 5 s, las lecturas exigen 100 ms
   estables y Confirmar solo se acepta si el pin se vio en alto alguna vez. Si
   GPIO39 queda en bajo permanente o GPIO36 nunca lee alto, la OLED muestra
   `GPIO39 EN BAJO` / `GPIO36 EN BAJO` y el arranque sigue normal. Un pull-up
   ausente ya no puede dejar el equipo colgado ni provocar un borrado solo.
2. **Portal:** sección "Mantenimiento del dispositivo", con confirmación escrita.
   - **Borrar solo WiFi:** exige escribir `WIFI`; borra credenciales STA y clave
     del AP. Conserva perfiles, canales, calibración, identidad y alarmas.
   - **Reset de fábrica:** exige escribir `RESET`; borra WiFi, clave del AP,
     backend, identidad/claim, perfiles local y del backend, canales habilitados,
     calibración, latches de alarma y eventos críticos pendientes.

El reset total borra los namespaces `cfg`, `calib` y `ws_crit`. La respuesta HTTP se envía
antes de reiniciar (1,5 s de margen) y los relés quedan OFF. Si el reset se hace
desde el portal en `MANTENIMIENTO`, también se detiene la prueba de canal.

No hay una acción parcial de "borrar identidad/token": si el backend revoca el
token, la única vía de re-provisionar es el reset total. Se evita así dejar el
equipo sin identidad, sin `claim_code` y sin forma de volver a reclamar sin
reiniciar.

## 12. Plan por hitos

### M0 — Base copiada y esqueleto (0.5 día)

- Copiar `src/`, `include/`, `lib/`, `platformio.ini` y `test/` de `RiegoControl`
  a `vivero_iot`, sin pisar `docs/`.
- Renombrar proyecto, banner, versión y nombres de clases.
- Actualizar `config.h`: 8 relés, pines nuevos, quitar el puente del 8.º canal,
  DHT21, un pin por DS18B20, sensor de ventana en GPIO13, alimentación conmutada
  en GPIO12, botones.
- Compilar con `pio run`.

**Acepta:** compila sin errores ni warnings nuevos; arranca; OLED y RTC
funcionan; los 8 relés están OFF; el AP del portal abre.

### M1 — 8 relés fijos y habilitación local (1–2 días)

- Enum de canales y capa `setRelay(Channel, bool)` con low-level.
- Habilitación por bitmask en NVS y checkboxes en el portal.
- Prueba individual desde el portal con auto-off.
- Arranque seguro y filtro de canales deshabilitados.
- Esqueleto del menú de botones.

**Acepta:** cada canal se habilita/deshabilita y persiste tras reiniciar; la
prueba solo actúa sobre habilitados; un canal deshabilitado no aparece en el
menú; al arrancar ningún relé pulsa.

### M2 — Sensores (2–3 días)

- DHT21 con validación y reintentos.
- Humedad Z1/Z2 con alimentación conmutada, 12 muestras y calibración dry/wet
  desde el portal.
- 3 DS18B20 con un pin por sonda (GPIO16, 32, 33), sin mapeo por dirección;
  verificación individual de cada sonda.
- Sensor de cerrada de ventana en GPIO13 con debounce y estados.
- Estructura de telemetría y páginas de OLED.

**Acepta:** lecturas plausibles y estables; la calibración persiste; desconectar
cada sensor produce `valid:false` sin bloquear el resto; una sonda caída no afecta
a las otras; los estados de ventana son correctos con un imán.

### M3 — Reglas y control local (3–4 días)

- Scheduler en hora local con RTC y deduplicación por entrada.
- Riego por umbral, interlocks, `max_run_s`, `min_off_s`.
- Termostato de cama con histéresis y `max_c`.
- Horarios de luz y ventilación; clima de ventilación.
- Máquina de estados de ventana con pulso toggle de 5 s, sensor de cerrada y
  timeout.
- Precedencia, timeouts de manual local y menú de botones.
- Evaluación offline con config cacheada.

**Acepta:** cada función responde a sensores simulados; el manual local preempta
web y automatización; los límites cortan; el pulso de ventana dura 5 s y nunca
queda continuo; un reinicio no duplica riegos; todo funciona sin WiFi.

### M4 — Alarmas (1–2 días)

- Motor multirregla con histéresis, retardo, latch y silencio.
- Persistencia de latches y eventos críticos.
- Silencio local con Confirmar 3 s.
- Advertencia de canal de alarma deshabilitado.

**Acepta:** tres reglas simultáneas se reflejan en el array; un latch sobrevive
el reinicio; una crítica nueva rompe el silencio; el relé sigue las reglas sin
backend.

### M5 — Portal y modos (2 días)

- DNS cautivo real, AP con WPA2 y clave configurable.
- Guardar WiFi sin reiniciar; automatización continúa en `PORTAL`.
- Mantenimiento con relés OFF y prueba temporizada.
- Editor del perfil local de horarios/umbrales y selector de fuente
  (`backend`/`local`).
- Campos de provisioning, estado, versión, diagnóstico y reset de fábrica.
- Timeout de 15 min y salida limpia a `NORMAL`.

**Acepta:** el teléfono abre el portal solo; guardar WiFi vuelve a `NORMAL` sin
reiniciar y sin cortar un riego; el override local cambia el comportamiento y
volver a backend aplica la última config recibida; la clave del AP se puede
cambiar; en mantenimiento no hay relés activos; el reset deja el equipo en
`PORTAL`.

### M6 — WebSocket (3–4 días)

- Cliente WS con headers de device/token y reconexión con backoff.
- `hello`, `welcome`, `config`, `telemetry`, `state`, `event`, `ack`, `ping`,
  `pong`, `request`, `error`.
- Cola offline, lotes, deduplicación y vencimiento de comandos.
- Sincronización de hora, persistencia de la config del backend y manejo del
  override local (guardar sin aplicar).
- Ring NVS de eventos críticos y backend falso (`tools/fake_backend.py`).

**Acepta:** conecta y reconecta; aplica config atómica; rechaza JSON inválido sin
perder la anterior; con override local, la config del backend se guarda y no se
aplica; el comando vencido no actúa; la cola se vacía al reconectar; no hay
telemetría duplicada; los eventos críticos sobreviven un reinicio; `--selftest`
del backend falso pasa; 72 h sin fugas ni reinicios.

### M7 — Integración y puesta en marcha (2–3 días)

- Prueba contra el backend real (MariaDB + API + WS).
- Verificación de comandos, ACK, eventos y configuración desde la web.
- Gabinete eléctrico, contactores, borneras, tierra y protecciones.
- Ensayo de carga real, 72 h de operación y documentación final.

**Acepta:** demo extremo a extremo; recuperación tras corte de energía y de
WiFi; alarmas y eventos visibles en la web; informe de 72 h sin fallas.

**Estimación total:** 14–20 días hábiles, sin contar bloqueos de hardware ni el
tiempo del equipo de backend.

## 13. Criterios de aceptación globales

1. **Offline 24 h:** horarios y umbrales se ejecutan; botones riegan, ventilan y
   mueven la ventana; la telemetría se bufferiza y se envía al reconectar.
2. **Cortes de energía (10 ciclos):** ningún relé pulsa; config, habilitación,
   calibración y latches persisten.
3. **Portal abierto:** un riego programado sigue su curso; al guardar WiFi se
   vuelve a `NORMAL` sin reiniciar.
4. **Config inválida:** se rechaza completa, se conserva la anterior y se emite
   `CONFIG_REJECTED` con errores.
5. **Alarmas:** varias simultáneas, latch y silencio según §9; sin backend.
6. **WebSocket:** reconexión < 60 s; comando LAN < 2 s; sin duplicados.
7. **Seguridad:** `max_run`, `max_c`, sensor inválido, timeout de ventana e
   interlock siempre se respetan, incluso con órdenes web.
8. **Memoria:** heap libre estable y sin fragmentación creciente en 72 h.
9. **Sin reinicios** no explicados en 72 h; `reset_reason` coherente.
10. **Config local:** editar un horario en el portal, activar override y verificar
    que se ejecuta; una config del backend posterior se guarda sin aplicarse; al
    volver a `backend` se aplica la última versión recibida.

## 14. Pruebas

### 14.1 Banco

- Módulo de 8 relés con 8 lámparas de bajo consumo o LEDs con resistencia.
- 3 DS18B20, DHT21, 2 potenciómetros en lugar de sensores de humedad, 1 reed
  switch para la ventana (y otro para probar v2), RTC y OLED.
- Pulsadores Selector/Confirmar/Configuración.
- Fuente de 5 V para relés y 3.3 V para lógica, con masa común.

### 14.2 Backend falso (`tools/fake_backend.py`)

Script Python que implementa el mínimo del contrato de `02_protocolo_ws.md`:

- `POST /api/v1/devices/claim` con validación de `claim_code` y respuesta
  `{token, device_id, ws_url, config_version}`.
- WebSocket `/ws/device` con headers `X-Device-Id` y `Authorization: Bearer`.
  Rechaza token inválido con cierre `4401` y una segunda sesión con `4409`.
- Responde `welcome` a `hello`, envía `config` completa, `ping` periódico y
  acepta `telemetry`, `state`, `event`, `ack`, `pong` y `error`.
- Consola para enviar `state`, `diag`, `ping`, `config`, `on/off`, `open/close`,
  `silence`, `reset` y JSON crudo.
- Log en color por dirección (`HTTP>`, `HTTP<`, `DEV>`, `SRV>`) y CSV opcional
  de telemetría.

Requisitos: Python 3.10+ y `websockets>=14`.

```powershell
cd C:\Users\javier\Desktop\OPENCODE PROYECTOS\vivero_iot
python -m pip install "websockets>=14"
python tools\fake_backend.py --http-port 8080 --ws-port 8081
```

El banner muestra la IP de LAN, el `claim_code` (`VIVERO-01` por defecto) y el
token. En el portal del equipo:

1. `backend_url`: `http://<IP-de-la-PC>:8080`
2. `claim_code`: `VIVERO-01`
3. Guardar y volver a `NORMAL`.

Secuencia esperada en el log:

```text
HTTP> claim device_id=vivero-XXXX claim_code=VIVERO-01 ...
HTTP< 200 device_id=vivero-XXXX ws_url=ws://192.168.x.x:8081/ws/device ...
WS    conectado ... device_id=vivero-XXXX session=s-...
DEV>  hello device=vivero-XXXX fw=0.1.0-m6 mode=NORMAL cfg=... source=backend ...
SRV>  welcome session=s-... heartbeat=25s telemetry=30s cfg=...
SRV>  config id=cfg-0100 version=100
DEV>  ack ref=cfg-0100 status=ok stored=True applied=True source=backend
DEV>  state mode=NORMAL ... 
DEV>  telemetry seq=... samples=1 ...
```

La consola acepta:

| Comando | Efecto |
|---|---|
| `state` / `diag` | pide `request what:"state"` / `"diagnostics"` |
| `ping` | envía un `ping` JSON y espera `pong` |
| `config [version]` | envía config; sin argumento sube la versión |
| `on riego_z1 60` | comando remoto con `expires_at` |
| `off riego_z1` | apaga el canal |
| `open` / `close` | pulso de ventana |
| `silence 600` / `reset` | alarma |
| `raw {...}` | mensaje JSON arbitrario para probar errores |
| `info` / `help` / `quit` | diagnóstico / ayuda / salir |

Prueba automática del propio backend (claim, auth, welcome, config, telemetría,
comando, ping y request):

```powershell
python tools\fake_backend.py --selftest
```

Para probar reconexión, cortar el proceso (`Ctrl+C`), esperar el backoff del
firmware y volver a levantarlo. El dispositivo debe reconectar, enviar `hello`,
`state` y el backlog de telemetría/eventos. Para probar el corte de Internet sin
apagar la PC, desconectar el WiFi del equipo o bloquear el puerto 8081 en el
firewall.


### 14.3 Casos mínimos

| # | Caso | Resultado esperado |
|---|---|---|
| T1 | Arranque con relés conectados | ninguno pulsa; todos OFF |
| T2 | Habilitar/deshabilitar canales | persiste; menú y automatización respetan |
| T3 | Horario de riego | dispara una vez; no duplica tras reinicio |
| T4 | Umbral de humedad | arranca tras 60 s; corta en `stop_above` |
| T5 | Manual local vs web | local gana; web `on` rechazado; `off` aceptado |
| T6 | `max_run_s` | corta y reporta `max_run` |
| T7 | Termostato de cama | banda, `max_c`, sensor inválido → OFF |
| T8 | Ventana | pulso de 5 s, sensor de cerrada, timeout y `FAULT` |
| T9 | Alarmas múltiples | array con 3 reglas; latch y silencio |
| T10 | Config inválida | rechazo atómico; versión anterior intacta |
| T11 | Sin WiFi 24 h | automatización y botones; buffer |
| T12 | Reconexión | `hello` → config → state → backlog sin duplicados |
| T13 | RTC inválido | sin horarios; umbral/termostato/manual OK |
| T14 | Soak 72 h | sin reinicios ni fuga de heap |
| T15 | Corte de energía | relés OFF; estado persistido |
| T16 | Config local + override | aplica local; backend se guarda sin aplicar; volver a backend |
| T17 | Sensor de ventana trabado | tras pulso sin transición → `FAULT`, sin reintento |
| T18 | Claim + WS con `fake_backend.py` | claim 200, `hello`→`welcome`→`config`, `ack` ok, telemetría y `state` visibles |
| T19 | Token o claim inválido | claim 403 / WS cierra 4401; el equipo sigue operando offline y no entra solo a `PORTAL` |
| T20 | Comando vencido o duplicado | `expired` o mismo `ack` sin reaplicar; el relé no cambia |
| T21 | Override local con backend | la config se guarda (`stored=true`, `applied=false`); al volver a `backend` se aplica |
| T22 | Corte y reconexión WS | backoff, `hello`, `state` y backlog de telemetría/eventos sin duplicados |
| T23 | Evento crítico + reinicio | el evento sobrevive en `ws_crit` y se reenvía con su `boot_id` original |
| T24 | Mantenimiento con ventana en movimiento | pulso abortado, estado igual al sensor y **sin** `WINDOW_FAULT` nuevo; un `FAULT` previo se conserva |
| T25 | Botón "Leer sensores" del portal | bloqueo de ~800 ms (no 2,4 s) y el ciclo siguiente reporta las 3 sondas **válidas** |
| T26 | Corte de riego fuera del ciclo de 30 s | `max_run_s` se respeta con precisión de ms; una zona no queda ON si el ciclo local no corre |
| T27 | Gesto de reset de fábrica completo | Configuración 10 s → soltar → Confirmar en 5 s → borra y arranca en `PORTAL` |
| T28 | Gesto sin pull-up en GPIO39/GPIO36 | el equipo **arranca igual** y muestra `GPIO39 EN BAJO` / `GPIO36 EN BAJO`; no queda colgado ni borra solo |
| T29 | Desmarcar los 7 días de un horario | guarda `daysMask = 0` y desactiva el horario; al reabrir el portal se ve desmarcado, no "todos los días" |
| T30 | Pulsar botones dentro de PORTAL/MANTENIMIENTO | al volver a `NORMAL` no se ejecuta nada pendiente: no reabre el portal ni entra al menú |
| T31 | Clave larga del AP en la OLED | SSID y clave se cortan con "..." a 18 caracteres; la clave derivada de la MAC (12) cabe completa |

### 14.4 Pruebas eléctricas

- Medir que IN1–IN8 estén en alto al arrancar (relés OFF).
- Verificar GPIO12 bajo al arrancar (pull-down 10 kΩ) y que IN1-IN8 estén en alto.
- Verificar que el relé 7 solo se energice 5 s por orden y vuelva a OFF.
- Probar cada canal con carga real y contactor/fusible adecuado.
- Medir temperatura de borneras y módulo en 1 h de operación.
- Verificar tierra, aislamiento y protección diferencial del gabinete.
- No conmutar 220 V directo con el módulo sin evaluar corriente y seguridad.

## 15. Decisiones tomadas y pendientes

| # | Decisión | Estado | Impacto |
|---|---|---|---|
| 1 | Actuador de ventana | **Tomada:** pulso toggle de 5 s, un relé | §8 y M3 de ventana |
| 2 | Sensores de ventana | **Tomada:** 1 sensor de cerrada en v1; 2.º en v2 | GPIO13; v2 requiere pin/expansor |
| 3 | Contrato WS y provisioning | **Tomada:** este proyecto propone `02`; backend confirma | M6 puede empezar con backend falso |
| 4 | Config local | **Tomada:** perfil local + override explícito | amplía portal M5 y lógica M6 |
| 5 | DS18B20 | **Tomada:** un pin por sonda (16/32/33) | M2, sin mapeo por dirección |
| 6 | Contraseña del AP | **Tomada:** configurable, default MAC | M5 |
| 7 | WSS/TLS | **Tomada:** `https://` + `wss://` con `setInsecure()` | falta validación de certificado (pinning) |
| 8 | ¿Alarma deshabilitable? | Pendiente | permitir con advertencia; evaluar forzar |
| 9 | Cargas reales y contactores | Pendiente | gabinete y seguridad eléctrica |
| 10 | Datasheet del controlador de ventana | Pendiente | polaridad y tiempo mínimo entre pulsos |
| 11 | Pin del 2.º sensor de ventana | Pendiente para v2 | GPIO15 boot-safe o expansor I2C |
| 12 | Borrado de identidad/token desde el portal | **Tomada:** no se implementa; la revocación se resuelve con reset de fábrica | §11.3 y `02_protocolo_ws.md` §11.3 |

## 16. Riesgos

- **Ventana toggle:** un pulso en el estado equivocado mueve la ventana al revés.
  Exigir estado conocido (`CLOSED`/`OPEN`), bloquear en `MOVING`/`FAULT` y no
  reintentar automáticamente. El datasheet del controlador sigue pendiente.
- **Sensor único de ventana:** no distingue “abierta” de “entreabierta” ni detecta
  un sensor trabado en “no cerrada”. Mitigación: prueba del sensor en el portal y
  `FAULT` si no hay transición tras el pulso; el 2.º sensor es v2.
- **GPIO5 (alarma):** es strapping pero **no impide el arranque** (los que sí lo
  hacen son GPIO0, GPIO2, GPIO12 y GPIO15). El riesgo real es otro: sin el
  pull-up de 10 kΩ, el relé de alarma queda en estado indefinido durante los
  primeros segundos de cada arranque. Verificar el pull-up y medir el IN.
- **GPIO12 (alimentación sensores):** strapping; debe estar bajo al arrancar. El
  pull-down externo es obligatorio, no opcional.
- **AP+STA:** el portal corta el WebSocket por diseño. La automatización sigue,
  pero el backend ve el equipo offline; es esperado y debe documentarse.
- **NVS:** muchas escrituras acortan la vida de la flash. Escribir solo cambios.
- **DS18B20 por pin:** tres cables y tres pull-ups; una sonda caída no afecta a
  las otras. Usar cable apantallado si el recorrido es largo y verificar que
  GPIO32/33 no queden flotantes.
- **Backend:** si el contrato o el provisioning se demoran, M6 se bloquea; el
  backend falso permite avanzar.
- **Hora local:** sin offset correcto, los horarios se corren una hora. El portal
  debe mostrar la hora local calculada para que el operador la valide.
- **220 V:** el módulo de relé no es un tablero eléctrico. Usar contactores,
  fusibles, bornera y gabinete; la seguridad eléctrica no es opcional.

