# Revisión del portal v2 — verificación en vivo

> **Revisado:** `/dashboard` de `web1.displaysur.online`, **2026-11-25**,
> dispositivo `vivero-DDAC03` en línea (fw `0.1.0-m6`, config v101).
> **Método:** navegación por las 9 pantallas, inspección del DOM/ARIA,
> medición de contraste y tamaños, consulta de `/dashboard/data` y
> verificación de las afirmaciones contra el código del firmware en `src/`.
>
> **Veredicto: la implementación es muy buena.** El rediseño cumple el
> `frontend_diseno.md` de punta a punta y la auditoría R1–R12 está aplicada y
> **medida**. Hay 3 defectos menores. El "hallazgo operativo" que reporté
> (§3) **quedó descartado**: era el banco de pruebas sin agua.

---

## 1. Verificado OK

### 1.1 Estructura y navegación

| Pantalla | Ruta | Contenido comprobado |
|---|---|---|
| Resumen | `/` | tarjetas con tendencia (`estable`, `↑ sube`, `↓ baja`) + **umbral al lado**, "En marcha ahora" con cuenta regresiva, "Todo en orden · última revisión hace 3 s" |
| Riego | `/riego` | tabs Z1/Z2/Aspersión, **editor de 4 horarios** con días L–D y duración en minutos, umbral con **humedad actual en vivo**, "Regar ahora" con 5/10/15/30 min, interlock/stagger, ayuda desplegable con las reglas reales |
| Clima | `/clima` | cama (termostato, "corte duro 35 °C no configurable", anti-ciclado 60/120 s), ventilación, iluminación |
| Ventana | `/ventana` | *"es un interruptor: solo abrir y cerrar, no hay detener"*, ajustes de motor |
| Alarmas | `/alarmas` | 3 tarjetas explicando Activa/Latched/Silenciada, **histórico con Inicio/Fin/Estado**, Silenciar 10 min, Resetear |
| Datos | `/datos` | rangos 1 h…30 d + rango personalizado, 2 gráficos, **tabla de estadísticas** (promedio/mín/máx/muestras), **Descargar CSV** |
| Eventos | `/eventos` | filtros tipo/severidad/canal/fechas, **220 eventos** (antes 25) |
| Equipo | `/equipo` | identidad, "Pedir diagnóstico", canales con **Habilitado (portal)** separado de **Automatización**, config con nota honesta de límites |
| Dispositivos | `/dispositivos` | lista con estado, último contacto, RSSI, alarmas |

### 1.2 Accesibilidad — medida en el DOM

| Requisito (auditoría R1–R12) | Medición | Estado |
|---|---|---|
| R1 `<h1>` por pantalla | `["Datos"]` | ✅ |
| R2 labels en todos los inputs | **0 sin label** de 3 | ✅ |
| R3 gráficos accesibles | **2/2 canvas con alternativa** + 3 `<table>` | ✅ |
| R4 regiones vivas | **2** (`aria-live` / `status` / `alert`) | ✅ |
| R5 landmarks + skip link | **4 landmarks** + "Saltar al contenido" | ✅ |
| R6 táctil 44 px | `@media (pointer: coarse) { min-height:44px; min-width:44px }` | ✅ |
| R7 jerga traducida | `IRRIGATION_STOP` → *"Riego zona 1: riego detenido"* | ✅ |
| R8 duración en minutos | "Duración (min) 10" + botones 5/10/15/30 | ✅ |
| R9 estado + tendencia | "16.0 % ↓ baja · Umbral para regar: 30 %" | ✅ |
| R10 estados vacíos | "Todo en orden · nada encendido ahora mismo · última revisión hace 3 s" | ✅ |
| R11 controles agrupados | por sistema, con confirmación | ✅ |
| Contraste (auditoría previa) | 13.59 / 7.87 / 13.35 : 1 | ✅ AAA |

### 1.3 Backend

- **R15 `GET /api/stats`** — tabla con 9 métricas × promedio/mín/máx/muestras ✅
- **R16 CSV** — botón presente; el texto aclara que descarga las muestras "tal cual" (sin promediar) ✅
- **R13/R17 retención** — pasó de 25 a **220 eventos** y la serie cubre **20 h**
  (24/11 18:33 → 25/11 14:50, 1660 muestras) ✅

---

## 2. Defectos encontrados

### D1 — Las estadísticas incluyen muestras inválidas 🟠

Medido en `/datos`:

| Métrica | Promedio | **Mínimo** | Máximo |
|---|---|---|---|
| Humedad Z1 (%) | 38.35 | **0** | 71.43 |
| Humedad Z2 (%) | 51.3 | **0** | 97.2 |
| Uptime (s) | 10981.36 | 32 | 28749 |

**Los dos problemas:**

1. **El mínimo `0 %`** viene de las muestras con `valid: false` (cuando el ADC
   leía 0 y el firmware marcaba el sensor como ausente). **Un 0 % no es una
   humedad real**: es un sensor caído. Contamina el promedio y el mínimo.
2. **`Uptime` no debería tener estadísticas**: es un contador monótono. Un
   "promedio de 10981 s" no significa nada y confunde en una tabla analítica.

**Fix:** excluir del cálculo de stats las filas donde la métrica vino `null` /
`valid: false`, y sacar `uptime_s` (y `rssi_dbm` si se quiere) del set de
métricas analizables.

### D2 — Footer duplicado 🟢

```
Vivero IoT · Vivero IoT — los horarios se evalúan en hora local del equipo…
```

El nombre de la app aparece dos veces. Es un detalle de plantilla.

### D3 — Texto "Sensor de cerrada: liberado" 🟢

En `/ventana`. "Liberado" es una traducción literal de *released* (el reed no
está accionado). Para el operador debería decir **"abierta"** o **"no cerrada"**,
que es lo que significa.

### D4 — Confirmado del lado del firmware: `rule[24]`

El equipo lo reportó y es correcto:

```c
char rule[24];                                   // ws_client.cpp:222 y 348
// clave más larga:
"humedad_persistente_baja"  = 24 caracteres  →  24 + NUL = 25 bytes
```

**Alcance real (verificado):** solo afecta `event.data.rule`. Las claves de
`state.alarms.active/latched` se arman directamente desde
`alarmConditionKey()` (sin pasar por el buffer), así que **esas están bien**.

**Fix:** `rule[24] → rule[32]` en las dos estructuras (`WsEvent` y
`CriticalEvent`).

---

## 3. Hallazgo operativo — ⚠️ descartado: condiciones de laboratorio

Durante la revisión reporté una anomalía en la zona 1. **El equipo aclaró que
es esperable en banco**, no un defecto:

> *"desestima el porcentaje de humedad de las zonas ya que estamos con pruebas
> de laboratorio y ocasionalmente la sonda puede salirse del medio en el que
> está, secarse muy rápido y mostrar una lectura errónea; por otro lado el
> riego aún no está conectado a la red de agua, por esa razón el riego no
> influye en la humedad de las zonas."*

**Evidencia observada (queda como registro):**

| Evidencia | Valor |
|---|---|
| Humedad Z1 | **16.0 %** y bajando (17.11 → 16.95 → 16.48 → 16.17) |
| Humedad Z2 | **66.4 %** |
| Riegos ejecutados | `15:29:51 → 15:30:02` (11 s) y `17:48:10 → 17:49:10` (60 s) |
| Efecto en la humedad | **ninguno**: siguió bajando después del riego |
| Histórico | *"Humedad de sustrato baja (60 min)"* repetida, una instancia de 4+ días |

**Por qué encaja con la explicación del equipo:**

1. La sonda sale del sustrato → mide aire → **seca rápido** → valor bajo y
   descendente, coherente con la temperatura ambiente.
2. **El riego no tiene agua**: el relé conmuta pero no moja, así que la
   humedad no puede recuperarse.
3. El relé **sí dispara** (`IRRIGATION_START`/`STOP`): la cadena de control
   funciona — detecta "seco", riega y corta.

**Conclusión: no hay nada que corregir** ni en el software ni en el hardware.
El comportamiento observado es el correcto para un banco sin agua.

**Lección de producto:** un sensor que se sale del medio produce una lectura
**válida pero falsa** (no cae en 0 ni en 4095), y el sistema no puede
distinguirla de una sequía real. En producción conviene calibrar con el
sustrato real y vigilar la **deriva**: si la humedad cae muy rápido sin riego
de por medio, es más probable una sonda fuera de lugar que una sequía
instantánea.

**Sobre la alarma `humedad_persistente_baja`:** no estaba activa al consultar
porque exige **120 ciclos consecutivos** (60 min) bajo el mínimo y el contador
se reinicia con cualquier valor por encima. Si la sonda oscila al salirse y
volver, el contador nunca llega a 120. El histórico confirma que **la alarma
funciona**: hay instancias registradas de hasta 7,7 h.

---

## 4. Correcciones a las notas del equipo

### 4.1 `paused` — la nota está desactualizada, la implementación es correcta ✅

Dicen: *"El paused real lo trae el firmware en M4."*

**El firmware ya lo tiene**, y su mapeo es el correcto:

```c
// ws_client.cpp:1486 y 1524
zone.paused = !(ch["auto_enabled"] | true);

// irrigation.cpp:327 — el manual se evalúa ANTES (línea 305)
bool* paused = getPaused(type, zone);
if (paused && *paused) return IrrigationTrigger::NONE;   // corta horario y umbral
```

`irrigationSetPaused()` / `irrigationIsPaused()` existen en `irrigation.h:109`.
**La adaptación CU-08 (`auto_enabled:false`) es exactamente la correcta**; solo
conviene actualizar la nota para que no parezca una limitación.

### 4.2 `enabled_channels` — ojo con la contradicción

La nota dice *"CU-19 habilitar canal es local (portal del equipo, por
contrato)"*, y `/equipo` lo implementa bien (columna "Habilitado (portal)"
separada de "Automatización").

**Pero el equipo está aplicando `enabled_channels` del backend**: hoy tiene
`cama_caliente` e `iluminacion` deshabilitados en el dispositivo
(`canales=0xE7`), coincidiendo con el `enabled_channels` del backend. Es decir,
el `config` **sí** cambia la habilitación local.

No es un error — es más útil así —, pero **hay que decidir cuál es la verdad**:
o el contrato dice "local y el backend solo lo muestra", o el backend puede
cambiarlo. Hoy hace las dos cosas y confunde.

### 4.3 `rule[24]` — correcto ✅

Confirmado, con el alcance acotado a `event.data.rule` (§2 D4).

---

## 5. Pendientes que siguen abiertos (no son de esta entrega)

| # | Tema | Estado |
|---|---|---|
| 1 | Job de poda / retención definitiva | decisión de producto |
| 2 | Roles de usuario (¿quién puede editar config?) | decisión |
| 3 | TLS pinning | decisión de seguridad |
| 4 | `rule[24] → rule[32]` en firmware | ✅ **hecho** (commit `caa0426`), falta flashear |
| 5 | Stats sin muestras inválidas (D1) | fix pequeño |
| 6 | ~~Verificar `min_humidity` aplicado~~ | ✅ **descartado**: la anomalía era el banco de pruebas (§3) |
| 7 | Calibrar las sondas con el sustrato real (no en aire) | al instalar el riego |

---

## 6. Nota sobre el proceso

El documento anterior falló por estar organizado por esquemas de API.
`frontend_diseno.md` funcionó porque está organizado por **casos de uso con
pasos**. La evidencia: las pantallas implementadas siguen exactamente la
estructura de la §4 (Resumen → Riego → Clima → Ventana → Alarmas → Datos →
Eventos → Equipo → Dispositivos) y los textos de ayuda de `/riego` reproducen
casi literalmente la §3 "Qué significa cada campo".

**Para las próximas entregas, mantener ese formato**: caso de uso → pasos →
qué ve el usuario → qué regla aplica.
