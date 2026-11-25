# Diseño del nuevo frontend — por casos de uso

> **Para:** equipo de frontend (y backend, para saber qué API falta).
> **Fecha:** 2026-11-25. **Firmware:** `0.1.0-m6`. **Equipo real:** `vivero-DDAC03`.
>
> **Por qué existe este documento:** el informe anterior (`frontend.md`) estaba
> organizado por esquemas de API y no quedó claro. **Este está organizado por
> lo que el operador quiere hacer.** Empezá por la §2 (casos de uso) y la §3
> (los dos resueltos paso a paso). La §5 es la referencia técnica.
>
> **Companion técnico:** [`frontend.md`](frontend.md) (esquemas, catálogos,
> reglas) · [`tickets_backend.md`](tickets_backend.md) (API que falta, T0–T8).

---

## 1. Punto de partida: qué hay hoy

El portal actual es un **MVP de verificación**. Funciona, y hace bien lo que hace:

| ✅ Tiene | ❌ No tiene |
|---|---|
| Selector de dispositivo + online + modo + config_source | Ninguna pantalla de programación |
| 8 tarjetas de métricas en vivo | Edición de horarios o umbrales |
| 2 gráficos (temperaturas, humedad) con rango fijo | Histórico de alarmas |
| 8 tarjetas de canal con `enabled`/`on` | Auditoría paginada / filtrable |
| Botones ON/OFF por canal + duración global | Diagnóstico del equipo |
| Alarmas activas (solo texto) | Detalle de dispositivo |
| Últimos N eventos | Exportación de datos |
| — | Feedback de `ack` tras enviar un comando |

**Estado verificado el 2026-11-25** (esto ya funciona, no tocar):

```
online: true        last_seen: 12:15:55   now: 12:16:12   ← telemetría fluyendo
seriesLen: 240      ultimo: 12:15:54      ← T0 resuelto
canales: 6 de 8 habilitados (cama_caliente e iluminacion OFF, por config del backend)
alarmas: active []  latched []
eventos: IRRIGATION_START 08:59:24 → IRRIGATION_STOP 09:00:24  (riego de 60 s)
```

**El hueco fundamental:** hoy solo se puede *mirar* y *pulsar un botón*.
Todo lo demás — programar, umbrales, auditoría, diagnóstico — no existe.

---

## 2. Casos de uso del dispositivo

23 casos, agrupados por lo que el operador intenta hacer.

| ID | Caso | Prioridad |
|---|---|---|
| **Lectura** | | |
| CU-01 | Ver cómo está el vivero ahora mismo | 🔴 Alta |
| CU-02 | Ver la evolución de temperatura y humedad en el tiempo | 🔴 Alta |
| CU-03 | Ver qué alarmas hay y en qué estado están | 🔴 Alta |
| CU-04 | Enterarse de qué pasó y cuándo (auditoría) | 🟡 Media |
| CU-05 | Saber si el equipo está sano y conectado | 🟡 Media |
| **Programación** | | |
| CU-06 | **Programar un riego diario para una zona** | 🔴 **Alta** |
| CU-07 | Que riegue solo cuando la humedad baja de un valor | 🔴 Alta |
| CU-08 | Pausar el riego de una zona sin borrar sus horarios | 🟡 Media |
| CU-09 | Programar la iluminación (encendido/apagado) | 🔴 Alta |
| CU-10 | Programar la ventilación | 🟡 Media |
| CU-11 | Poner la cama caliente a una temperatura | 🔴 Alta |
| CU-12 | Configurar los umbrales de ventilación | 🟡 Media |
| CU-13 | Ajustar los límites de alarma de temperatura | 🟡 Media |
| **Control** | | |
| CU-14 | Regar una zona ahora mismo, por un tiempo | 🔴 Alta |
| CU-15 | Encender/apagar luz o ventilador a mano | 🟡 Media |
| CU-16 | Abrir o cerrar la ventana | 🟡 Media |
| CU-17 | Poner la cama en automático / forzar / apagar | 🔴 Alta |
| CU-18 | Silenciar o resetear una alarma | 🔴 Alta |
| **Administración** | | |
| CU-19 | Habilitar o deshabilitar un canal | 🟡 Media |
| CU-20 | Configurar el equipo (WiFi, backend, clave AP) | 🟡 Media |
| CU-21 | Gestionar varios viveros / dispositivos | 🟡 Media |
| CU-22 | Diagnosticar un problema | 🟢 Baja |
| CU-23 | Exportar los datos para análisis externo | 🟢 Baja |

---

## 3. Los dos casos clave, resueltos paso a paso

### CU-06 — Programar un riego diario para una zona

> **Pregunta real:** *¿cómo programo un riego diario para una zona y cuánto
> durará el riego?*

**Respuesta corta:** pantalla **Riego → Zona Z1 → Horarios → + Nuevo horario**,
ponés `06:00`, duración `600 s`, días **L M X J V S D** y activo. Eso riega
**10 minutos cada día a las 06:00**.

#### Pasos exactos

```
1. Pestaña "Riego"
2. Sub-pestaña "Z1"            (o Z2 / Aspersión)
3. Sección "Horarios" → botón "+ Nuevo horario"
4. Completar:
      Hora de inicio   06:00
      Duración         600 segundos   (= 10 minutos)
      Días             [x]Lu [x]Ma [x]Mi [x]Ju [x]Vi [x]Sa [x]Do
      Activo           [x]
5. "Guardar"
6. El backend incrementa config_version y manda `config` al equipo
7. El equipo responde `ack` → la UI muestra "Programado ✓"
```

#### Qué significa cada campo (y esto es lo que hay que explicar en la UI)

| Campo | Ejemplo | Qué significa realmente |
|---|---|---|
| `start` | `06:00` | **Hora local** (UTC−3 por defecto), no UTC. Se evalúa con el RTC |
| `duration_s` | `600` | **Cuánto tiempo queda encendido** el relé, en segundos. 600 s = 10 min |
| `days_mask` | `127` (0x7F) | 7 bits, lunes = bit 0. **Los 7 desmarcados = horario desactivado** |
| `enabled` | `true` | Si está en `false`, el horario existe pero no se evalúa |

#### Qué pasa en el equipo a las 06:00

```
06:00:00  evaluateZone → SCHEDULE coincide → ON   (evento IRRIGATION_START, reason=schedule)
06:10:00  elapsed >= durationMs (600 s)   → OFF  (evento IRRIGATION_STOP, reason=done)
```

**La duración es por disparo.** Si el horario es todos los días, son 10 min
todos los días.

#### Respuestas a las dudas típicas

| Duda | Respuesta |
|---|---|
| **¿Cuánto durará?** | Lo que pongas en `duration_s`, **1…3600 s**. Se recorta a `max_run_substrate_s` (900 s por defecto); si pedís más, se corta y dispara `irrigation_timeout` |
| **¿Y si a las 06:00 ya está regando otro?** | El **interlock** permite un solo riego a la vez (Z1+Z2+aspersión). El segundo queda en espera y emite `IRRIGATION_SKIPPED (interlock)`. Además hay `stagger_s` (30 s) entre zonas distintas |
| **¿Se solapan dos horarios?** | Gana el primero; el segundo se omite con `IRRIGATION_SKIPPED (overlap)` |
| **¿Y si no hay agua/humedad?** | El **horario manda**: riega igual. El umbral es **otra fuente**, no una condición del horario |
| **¿Puedo pausar sin borrar?** | Sí: `paused` en la zona. Pausa horario **y** umbral; el manual sigue funcionando |
| **¿Se corta solo?** | Sí, al llegar a `duration_s` o a `max_run_s` |
| **¿Y si reinicio el equipo?** | No se pierde: está en NVS. Se re-evalúa al arrancar |
| **¿Y si el RTC no tiene hora?** | **No ejecuta horarios.** El umbral y el manual sí. La UI debe avisar (`clock.valid: false`) |

#### Las 3 fuentes de riego y su prioridad

Esto es lo que más confunde. El equipo elige **una sola** por zona, en este orden:

```
1. MANUAL_LOCAL    botón en el equipo / portal del equipo   ← gana siempre
2. OVERRIDE        comando desde la web
3. PAUSED          si está pausado → no riega por horario ni umbral
4. SCHEDULE        horario programado
5. THRESHOLD       humedad bajo el mínimo (Z1/Z2) o calor (aspersión)
```

**Consecuencia para la UI:** si `source = manual_local`, un `on` de la web se
rechaza con `local_manual_active`. **Mostrar "control local", no un error.**

---

### CU-07 — Riego por humedad (umbral)

```
Riego → Z1 → "Umbral de humedad"
   Humedad mínima para regar   30 %
   Regar automáticamente por umbral   [x]
```

**Qué hace el equipo:** si la humedad baja de **30 % durante 60 s seguidos**
(2 ciclos locales) → ON. Corta cuando sube de `stop_above`. Espera `min_off_s`
(1800 s por defecto) antes de poder volver a disparar por umbral.

**Diferencias con el horario:**

| | Horario | Umbral |
|---|---|---|
| Se dispara | a una hora concreta | cuando la humedad cae |
| Requiere hora válida | **sí** | no |
| Puede ir todos los días | sí | siempre (si está habilitado) |
| Espera `min_off_s` | no | **sí** |
| Zonas | Z1, Z2, aspersión | Z1, Z2 (aspersión usa temperatura) |

**En la UI:** el umbral debe mostrar la **humedad actual** al lado del
valor configurado, para que el operador vea cuánto falta.

---

## 4. Diseño: arquitectura de información

```
Vivero IoT
│
├── 🏠 Resumen ...................... CU-01, CU-03, CU-05
│     estado en vivo · gráficos · alarmas · accesos rápidos
│
├── 💧 Riego ........................ CU-06, CU-07, CU-08, CU-14
│     ├── Z1   [Estado | Horarios | Umbral]
│     ├── Z2   [Estado | Horarios | Umbral]
│     ├── Aspersión [Estado | Horarios]
│     └── Ajustes globales (interlock, max_run, min_off, stagger)
│
├── 🌡️ Clima ........................ CU-09, CU-10, CU-11, CU-12, CU-13
│     ├── Iluminación [Estado | Horarios]
│     ├── Ventilación [Estado | Horarios | Umbrales]
│     └── Cama caliente [Setpoint | Automático]
│
├── 🪟 Ventana ....................... CU-16
│
├── 🔔 Alarmas ....................... CU-03, CU-18
│     Activas · Latched · Histórico
│
├── 📈 Datos .......................... CU-02, CU-23
│     Serie temporal con rangos · exportar CSV
│
├── 📋 Eventos ....................... CU-04
│     Filtro por código/severidad/fecha · paginación
│
├── ⚙️ Equipo ........................ CU-05, CU-19, CU-20, CU-22
│     Estado · Diagnóstico · Canales · Configuración
│
└── 📱 Dispositivos .................. CU-21
      Lista con estado · alta/baja
```

**Regla de diseño:** cada pantalla de programación tiene **4 bloques fijos** —
*Estado actual* → *Configuración* → *Guardar* → *Confirmación con el `ack`*.
El operador siempre debe ver, en el mismo lugar, **qué está pasando ahora**
y **qué va a pasar después**.

---

## 5. Detalle por pantalla

### 5.1 🏠 Resumen (CU-01, CU-03, CU-05)

```
┌──────────────────────────────────────────────────────────────┐
│ [vivero-DDAC03 ▾]  ● En línea   Modo: NORMAL   Config: backend│
│                            Último contacto: hace 4 s  [Actualizar]│
├──────────────────────────────────────────────────────────────┤
│ Temp. aire    Humedad aire   Humedad Z1   Humedad Z2          │
│  24.7 °C       45.8 %        29.0 %       36.3 %              │
│ Temp. cama    Sustrato Z1    RSSI         Ventana             │
│  24.2 °C       24.3 °C       -75 dBm      open                │
├─────────────────────────────┬────────────────────────────────┤
│ 📈 Temperaturas (°C)        │ 📈 Humedad (%)                  │
│   [1h][6h][24h][7d][30d]   │   [1h][6h][24h][7d][30d]        │
├─────────────────────────────┴────────────────────────────────┤
│ 💧 Riego Z1  ● ON  fuente: schedule   restan 4:32   [Detener]│
│ 💧 Riego Z2  ○ off                                         │
│ 🌡️ Cama      ○ off  fuente: auto     setpoint 25.0 °C        │
├──────────────────────────────┬───────────────────────────────┤
│ 🔔 Alarmas (2)               │ 📋 Últimos eventos             │
│  • Cama caliente por encima  │  IRRIGATION_STOP  riego_z1     │
│    [LATCHED] [Resetear]      │  IRRIGATION_START riego_z1     │
│  • Sensor Z1 fuera de servicio│ WS_CONNECTED                   │
│    [Silenciar 10 min]        │  WIFI_CONNECTED                │
└──────────────────────────────┴───────────────────────────────┘
```

**Detalle importante — la tarjeta de riego activo.** Es la pieza que falta hoy
y la que más valor da: el operador necesita ver **que el riego está corriendo,
de quién es, y cuándo termina**.

### 5.2 💧 Riego — por zona (CU-06, CU-07, CU-08, CU-14)

```
┌─ Riego ──────────────────────────────────────────────────────┐
│ [Z1] [Z2] [Aspersión]                          Zona: Sustrato │
├── Estado ────────────────────────────────────────────────────┤
│  ● APAGADO      fuente: —        Último: hoy 06:00–06:10      │
│  [ Regar ahora ▾ 5 min ]   ← CU-14, con duración elegible    │
├── Horarios ───────────────────────────────────────────────────┤
│  #  Inicio  Duración  Días                      Activo  [x] │
│  1  06:00   10 min    Lu Ma Mi Ju Vi Sa Do        [x]   [✎][🗑]│
│  2  18:00    5 min    Lu Ma Mi Ju Vi               [x]   [✎][🗑]│
│  3  —        —         —                            [ ]        │
│  4  —        —         —                            [ ]        │
│  [+ Nuevo horario]                                           │
├── Umbral de humedad ─────────────────────────────────────────┤
│  Actual: 29.0 %      Mínimo para regar: [30] %                │
│  [x] Regar automáticamente por umbral                         │
│  Último disparo por umbral: ayer 14:20 (espera 22 min)        │
├── Ajustes de esta zona ──────────────────────────────────────┤
│  Duración base: [600] s     Pausar automatización: [ ]        │
├──────────────────────────────────────────────────────────────┤
│                              [ Guardar cambios ]              │
└──────────────────────────────────────────────────────────────┘
```

**Lo que hay que dejar claro en la interfaz:**

1. **"Duración" es cuánto dura CADA riego**, no la frecuencia. La frecuencia
   la dan los días y el número de horarios.
2. **Hay 4 slots de horario por zona.** No ilimitados.
3. **Mostrar la humedad actual junto al umbral** — sin eso el número no
   significa nada.
4. **Mostrar `min_off` en curso** si el operador intenta regar por umbral y
   todavía está en espera.

### 5.3 🌡️ Clima (CU-09 a CU-13)

```
┌─ Clima ──────────────────────────────────────────────────────┐
│ [Iluminación] [Ventilación] [Cama caliente]                   │
├── Cama caliente ─────────────────────────────────────────────┤
│  Temperatura actual: 24.2 °C                                  │
│  Setpoint: [25.0] °C   (corte duro 35.0 °C, no editable)      │
│  [x] Automático                                               │
│  Estado: ● apagado · fuente: auto                             │
│  [ Encender 30 min ]  [ Apagar ]  [ Automático ]              │
│  Anti-ciclado: mínimo 60 s encendido / 120 s apagado          │
├── Ventilación ───────────────────────────────────────────────┤
│  [x] Automático   [x] Por clima (si no, solo por horario)     │
│  Encender con:  temp ≥ [30.0] °C   O   humedad ≥ [80.0] %     │
│  Apagar con:    temp ≤ [28.0] °C   Y   humedad ≤ [70.0] %     │
│  Actual: 24.7 °C / 45.8 %  →  no se enciende                 │
│  Límites: mín 60 s ON · mín 120 s OFF · máx 1 h continuas     │
├── Iluminación ───────────────────────────────────────────────┤
│  Horarios (4 slots, mismo editor que riego)                   │
│  Corte duro: 16 h continuas, aunque la config pida más        │
└──────────────────────────────────────────────────────────────┘
```

**Nota clave para la UI:** la ventilación usa **OR** entre temperatura y
humedad para encender, pero **AND** (las dos por debajo) para apagar. Y necesita
**2 ciclos consecutivos** (60 s) antes de decidir. La UI debe mostrar
"decidiendo…" si aplica.

### 5.4 🔔 Alarmas (CU-03, CU-18)

```
┌─ Alarmas ────────────────────────────────────────────────────┐
│ [Activas (2)] [Latched (1)] [Histórico]                       │
├──────────────────────────────────────────────────────────────┤
│ 🔴 Cama caliente por encima del máximo     hace 12 min        │
│    valor: 35.4 °C  ·  límite: 35.0 °C                         │
│    [LATCHED]  ← queda hasta que alguien la resetee            │
│    [ Resetear ]                                               │
│                                                              │
│ 🟠 Sensor de sustrato Z1 fuera de servicio   hace 3 h         │
│    [ Silenciar 10 min ]  ← silencia el relé, NO borra         │
└──────────────────────────────────────────────────────────────┘
```

**Tres estados que no se deben mezclar:**

| Estado | Significado | UI |
|---|---|---|
| **Activa** | la condición se cumple ahora | badge rojo |
| **Latched** | la condición pasó pero quedó pegada | badge distinto + botón Reset |
| **Silenciada** | el relé dejó de sonar 10 min, la condición sigue | "silenciada hasta 14:30", **no** "resuelta" |

**Hay 11 condiciones y pueden estar varias a la vez.** Nunca mostrar "la"
alarma; siempre una lista.

---

## 6. Mapa caso de uso → API necesaria

Aquí es donde se ve qué falta. **Detalle en [`tickets_backend.md`](tickets_backend.md).**

| Caso | API que necesita | Estado |
|---|---|---|
| CU-01, CU-02 | `GET /dashboard/data` + rango en `series` | ✅ parcial (rango fijo) → **T7** |
| CU-03, CU-18 | `GET /api/alarms` con histórico | ❌ → **T2** |
| CU-04 | `GET /api/events` paginado y filtrable | ❌ → **T3** |
| CU-05, CU-22 | `GET /api/diagnostics` | ❌ → **T6** |
| **CU-06, CU-07, CU-08** | **`GET/PUT /api/config`** | ❌ → **T1** |
| **CU-09 a CU-13** | **`GET/PUT /api/config`** | ❌ → **T1** |
| CU-14 a CU-17 | `POST /dashboard/command` + `ack` | ⚠️ sin `ack` → **T8 / T4** |
| CU-19, CU-20 | `GET/PUT /api/config` | ❌ → **T1** |
| CU-21 | `GET /api/devices` con estado | ❌ → **T5** |
| CU-23 | `GET /api/telemetry` por rango + export | ❌ → **T7** |

**Conclusión para el equipo:** **T1 es el 60 % del producto.** Sin
`GET/PUT /api/config` no se puede programar nada, que es justamente lo que
pregunta el usuario.

---

## 7. Reglas de negocio que el frontend debe respetar

Estas son las que rompen la UI si no se aplican:

1. **`valid: false` → "sin dato", nunca `0`.** Los números llegan como
   `string` (`"24.7"`); convertir antes de graficar.
2. **La duración es por disparo**, no la frecuencia. La frecuencia la dan
   días + horarios.
3. **`days_mask` con los 7 días desmarcados = horario desactivado**, no "todos".
4. **Prioridad local:** con `source: manual_local` la web no puede encender.
   Es diseño, no error.
5. **Config atómica:** si un campo está fuera de rango, el equipo responde
   `CONFIG_REJECTED` y **no aplica nada**. Validar en cliente antes de enviar.
6. **`stop` no existe en la ventana** (`window_stop: false`). Es toggle:
   `open` / `close` únicamente.
7. **El corte de la cama a 35 °C no es configurable** y no se puede anular
   ni con manual.
8. **Sin RTC válido no corren los horarios.** Avisar en la UI.
9. **La ventana solo confirma "cerrada"** (`window_limits: 1`): `open` no
   significa posición conocida.
10. **Rangos:** duración 1…3600 s · `min_off` ≤ 43200 s · `stagger` ≤ 600 s ·
    umbral 0…100 % · setpoint 5…34.9 °C.

---

## 8. Plan de ejecución por fases

| Fase | Qué entrega | Casos | Depende de |
|---|---|---|---|
| **F1** | Tarjeta de riego activo + `ack` visible en los controles | CU-14 a CU-18 | **T8** |
| **F2** | Pantalla de programación de riego | **CU-06, CU-07, CU-08** | **T1** |
| **F3** | Pantalla de clima (cama, ventilación, luz) | CU-09 a CU-13 | **T1** |
| **F4** | Alarmas con histórico + botones silenciar/reset | CU-03, CU-18 | **T2** |
| **F5** | Eventos filtrables y paginados | CU-04 | **T3** |
| **F6** | Selector de dispositivo con estado | CU-21 | **T5** |
| **F7** | Rangos de gráficos + export CSV | CU-02, CU-23 | **T7** |
| **F8** | Diagnóstico y configuración del equipo | CU-05, CU-19, CU-20, CU-22 | **T6, T1** |

**F1 se puede hacer ya** (el endpoint de comando existe; solo falta que
devuelva el `ack`). **F2 y F3 están bloqueadas en T1.**

---

## 9. Cómo validar que el frontend está bien

Casos de prueba operativos — simulan al usuario, no al código:

| # | Hago esto... | Y debe pasar esto |
|---|---|---|
| P1 | Programo Z1 a las 06:00, 600 s, todos los días | aparece "Programado ✓" y el equipo lo confirma con `ack ok` |
| P2 | Pongo un horario a la hora actual + 1 min | a los 60 s la tarjeta pasa a **ON** con cuenta regresiva y aparece `IRRIGATION_START (schedule)` |
| P3 | Dejo correr hasta el final | se apaga sola, `IRRIGATION_STOP (done)`, sin alarma |
| P4 | Pido 4000 s de duración | la UI lo rechaza antes de enviar (rango 1…3600) |
| P5 | Pongo dos zonas a la misma hora | la segunda espera (`interlock`) y lo dice en la UI, no como error |
| P6 | Desactivo los 7 días de un horario | queda desactivado, **no** "todos los días" |
| P7 | El equipo está en control local y pulso ON | muestra "control local", no un error |
| P8 | Desconecto el equipo 10 min | badge "desconectado", gráficos con hueco marcado |
| P9 | Llega una alarma con latch | aparece en "Latched" con botón Reset; resetear la limpia |
| P10 | Silencio una alarma | dice "silenciada hasta 14:30", **no** "resuelta" |
| P11 | El RTC pierde la hora | aviso visible y los horarios no se ejecutan |
| P12 | Envío un comando y el equipo lo rechaza | el `ack` explica por qué, sin reenviar |
