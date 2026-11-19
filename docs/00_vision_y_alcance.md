# Vivero IoT — Visión, alcance y decisiones (v0.1)

**Fecha:** 16/11/2026
**Proyecto base:** `C:\Users\javier\Desktop\FITBA 2\RiegoControl` (revisado 16/11/2026)
**Estado:** Diseño v0.1 con las decisiones de hardware cerradas. Queda pendiente
acordar el alta/token con el equipo de backend y el datasheet del controlador de
ventana.

## 1. Objetivo

Dispositivo ESP32 para monitorear las variables ambientales de un vivero y controlar
actuadores de riego, clima e iluminación. Debe operar desde una web remota (backend
WebSocket) y, sin conexión, seguir funcionando con la última configuración válida y
control manual local.

## 2. Canales de salida (fijos en firmware)

| # | Canal | Tipo interno | GPIO | Notas |
|---|---|---|---|---|
| 1 | Riego zona 1 (sustrato) | `RIEGO_SUSTRATO_1` | 26 | Relé low-level |
| 2 | Riego zona 2 (sustrato) | `RIEGO_SUSTRATO_2` | 27 | Relé low-level |
| 3 | Riego por aspersión | `RIEGO_ASPERSION` | 18 | Relé low-level |
| 4 | Cama caliente | `CAMA_CALIENTE` | 14 | Relé low-level |
| 5 | Iluminación | `ILUMINACION` | 19 | Relé low-level |
| 6 | Ventilación forzada | `VENTILACION` | 17 | Relé low-level |
| 7 | Ventana | `VENTANA` | 23 | Relé low-level; pulso de 5 s a controlador toggle |
| 8 | Alarma | `ALARMA` | 5 | Relé low-level; pull-up obligatorio (ver 01 §3) |

Reglas:

- La función y el pin de cada canal están **fijos en firmware**. El portal no los
  cambia; solo permite **habilitar/deshabilitar**, **probar** y editar el perfil
  local de horarios/umbrales.
- Un canal deshabilitado no se evalúa en automatización y no aparece en el menú
  manual de los botones.
- El canal de alarma es una salida del módulo de alarmas; no forma parte del menú
  manual (solo prueba desde el portal).
- Todos los relés arrancan **OFF**. Toda escritura pasa por
  `setRelay(Channel, bool active)` con `RELAY_ACTIVE_LOW`.
- El módulo es opto-acoplado low-level trigger (confirmado con hardware real en la
  base). Pull-up externo de 10 kΩ por entrada IN.

## 3. Sensores

| Sensor | Variable | Interfaz | GPIO | Notas |
|---|---|---|---|---|
| DHT21 | Temperatura y humedad de aire | 1-wire propietario | 4 | Pull-up 10 kΩ |
| Humedad sustrato Z1 | Humedad de suelo 0-100 % | ADC1 + alimentación conmutada | 34 | Calibración dry/wet |
| Humedad sustrato Z2 | Humedad de suelo 0-100 % | ADC1 + alimentación conmutada | 35 | Calibración dry/wet |
| Temp. sustrato Z1 | Temperatura de suelo | DS18B20 / OneWire | 16 | Un pin por sonda |
| Temp. sustrato Z2 | Temperatura de suelo | DS18B20 / OneWire | 32 | Un pin por sonda |
| Temp. cama caliente | Temperatura de cama | DS18B20 / OneWire | 33 | Un pin por sonda |
| Estado de ventana | Cerrada / no cerrada | 1 sensor de cerrada | 13 | v1; v2 evalúa 2.º sensor |
| Alimentación sensores humedad | ON/OFF | Salida | 12 | XY-MOS; pull-down externo |

- **DS18B20:** un pin por sonda (decisión del usuario): GPIO16 (Z1), GPIO32 (Z2),
  GPIO33 (cama). Cada sonda con su pull-up de 4.7 kΩ. No se usa bus compartido.
- **Ventana:** v1 usa **un solo sensor de cerrada** en GPIO13. El controlador del
  motor maneja los límites internos; el sensor solo confirma cerrada. En v2 se
  puede agregar un segundo sensor si se libera un pin o se usa un expansor I2C.
- **Alimentación conmutada:** se mueve de GPIO13 a **GPIO12** para liberar GPIO13
  para el sensor de ventana. GPIO12 es strapping (debe estar bajo al arrancar):
  lleva pull-down externo de 10 kΩ y alimenta los sensores solo después del boot.
- **GPIO2** queda libre (strapping); no se usa en v0.1. GPIO15 también queda libre
  (strapping) y no se usa.
- No se incluyen luz, CO2, nivel ni caudal en v0.1.

## 4. Modos de operación

| Modo | WiFi | Automatización | Portal | Uso |
|---|---|---|---|---|
| `NORMAL` | STA | Sí | No (o solo diagnóstico) | Operación normal con backend |
| `PORTAL` | AP | **Sí, con última config** | Sí, cautivo | Configuración / WiFi / habilitar relés |
| `MANTENIMIENTO` | AP | No (relés OFF) | Sí | Intervención segura (opcional) |

- **No se usa AP+STA simultáneo** (se conserva la regla de la base). Al abrir el
  portal se corta la conexión WebSocket; la automatización sigue con la última
  configuración cacheada.
- Al salir del portal se reintenta STA y WebSocket sin reiniciar el equipo
  (mejora respecto de la base, que reiniciaba).
- Entrada al portal: botón Configuración, o falla de WiFi > 60 s, o primer arranque
  sin credenciales.
- Timeout de inactividad del portal: 15 min → volver a `NORMAL` sin reiniciar.
- **La automatización no se suspende en modo portal** (decisión del usuario). Solo
  se suspende en `MANTENIMIENTO` explícito.

## 5. Comportamiento sin conexión

- Se cachea en NVS la última configuración válida recibida por WebSocket:
  horarios, umbrales, setpoint/histéresis de cama, umbrales de alarma y
  `config_version`.
- Además se guarda un **perfil local** editable desde el portal (horarios y
  umbrales). Por defecto manda la config del backend; si nunca hubo una, manda el
  perfil local. El operador puede activar un **override local** explícito desde el
  portal; mientras esté activo, el backend sigue enviando config pero el equipo la
  guarda sin aplicarla, y la web lo ve en `state.config_source`.
- El RTC DS3231 mantiene la hora. NTP la corrige cuando hay WiFi. Los horarios se
  evalúan en **hora local** (offset configurable, por defecto UTC-3); las marcas de
  telemetría/eventos van en UTC ISO 8601.
- Riego por horario: se ejecuta con los últimos horarios recibidos.
- Riego por umbral: funciona si fue configurado y habilitado.
- Riego manual: botones Selector/Confirmar, solo canales habilitados.
- Control manual de ventilación y ventana: botones Selector/Confirmar.
- Telemetría y eventos se acumulan en buffers FIFO acotados; al reconectar se
  envían. Si se llenan, se descarta el más antiguo.
- Comandos remotos con `expires_at`: si expiran antes de aplicarse, se descartan y
  se reporta `expired`.

## 6. Backend

- Otro equipo: MariaDB + API + WebSocket.
- El dispositivo es **cliente WebSocket** y envía/recibe JSON. No expone la base.
- Contrato propuesto en `02_protocolo_ws.md` (a acordar con el equipo de backend).
- El dispositivo envía: `hello`, `telemetry`, `event`, `state`, `ack`, `pong`.
- El dispositivo recibe: `welcome`, `config`, `command`, `request`, `ping`, `error`.
- Reconexión con backoff, heartbeat y cola offline.
- **Pendiente:** cómo se provisiona `device_id`/`token` (por WS, por portal o por
  HTTP). **Recomendado:** `claim_code` + `POST /devices/claim`; el equipo de
  backend debe confirmar el endpoint.

## 7. Seguridad y fail-safe

- Relés OFF al arrancar; estado seguro ante reset/watchdog.
- Watchdog de tarea y recuperación de WiFi/WS.
- Límites por canal: tiempo máximo encendido, tiempo mínimo apagado, histéresis.
- Cama caliente: corte por temperatura máxima y por sensor inválido.
- Ventana: pulso de 5 s, sensor de cerrada y timeout de recorrido; si no hay
  transición, se detiene y se genera alarma.
- Alarmas por OR: temperatura de aire fuera de rango, cama sobre-temperatura,
  falla de sensores, falla de riego, ventana sin transición, sin conexión
  (configurable).
- Cargas de 220 V: contactor/fusible/tierra/gabinete. No conmutar potencia directa
  con el módulo de relé sin evaluar la carga y la seguridad.
- AP del portal con contraseña WPA2 **configurable**; la clave inicial se deriva
  de la MAC y se muestra en el OLED. No hay AP abierto.

## 8. Fuera de alcance v0.1

- Validación de certificado TLS (el cifrado ya está: `https://` + `wss://` con
  `setInsecure()`).
- Autenticación de usuarios web (la define el equipo de backend).
- Sensores de luz, CO2, nivel de tanque o caudal.
- OTA (actualización por USB).
- App móvil nativa.
- Dashboard web local en modo STA (solo portal AP), salvo decisión contraria.

## 9. Decisiones tomadas y pendientes

### 9.1 Tomadas

1. **Actuador de ventana:** controlador propio que acepta un **pulso de 5 s** en el
   relé y **alterna** el sentido (toggle). Un solo relé alcanza.
2. **Estado de ventana:** v1 con **un sensor de cerrada** (GPIO13). El controlador
   maneja los límites internos; el firmware confirma cerrada y deduce abierta. En
   v2 se evalúa un segundo sensor si se libera un pin o se agrega un expansor I2C.
3. **Contrato WebSocket:** este proyecto propone `02_protocolo_ws.md`; el equipo de
   backend lo confirma o adapta. Alta recomendada por `claim_code` + HTTP.
4. **Config local:** el portal puede editar horarios y umbrales. La config del
   backend manda por defecto; el perfil local se usa si no hay config de backend o
   si el operador activa un override local explícito.
5. **DS18B20:** un pin por sonda (GPIO16, 32, 33), sin bus compartido.
6. **Sensor de ventana:** GPIO13; la alimentación conmutada de humedad pasa a
   GPIO12 con pull-down externo.
7. **Contraseña del AP:** configurable; default derivada de la MAC y visible en el
   OLED.
8. **Modos:** el portal no suspende la automatización; solo `MANTENIMIENTO` lo
   hace.

### 9.2 Pendientes

- Modelo/datasheet del controlador de ventana: polaridad del pulso, tiempo mínimo
  entre pulsos y comportamiento ante un pulso durante el movimiento.
- Pin para el segundo sensor de ventana en v2 (GPIO15 es strapping; evaluar
  expansor I2C).
- Endpoint de alta/token del backend y soporte de headers en el servidor WS.
- Validación de certificado TLS antes de exponer el equipo sin supervisión
  (hoy `setInsecure()`).
- Cargas reales, contactores y protecciones del gabinete.
- ¿La alarma debe poder deshabilitarse desde el portal? Hoy se permite con
  advertencia.

## 10. Hallazgos de la base a corregir

- `cloud.cpp`: comparación `"aspeersion"` (typo) en lugar de `"aspersion"`.
- Horarios: la base evalúa `HH:MM` contra UTC; en el nuevo proyecto se evalúan en
  hora local.
- Modo CONFIG de la base suspende toda la automatización; el nuevo proyecto no.
- Alarmas: la base mantiene una sola condición activa; el nuevo proyecto debe
  soportar varias simultáneas.
- Portal: la base reinicia al guardar; el nuevo proyecto vuelve a NORMAL sin reiniciar.
- AP de la base abierto; el nuevo proyecto usa WPA2.
