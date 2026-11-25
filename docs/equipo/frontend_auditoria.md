# Auditoría — Simpleza, accesibilidad y capacidad de análisis

> **Objeto:** `/dashboard` de `web1.displaysur.online`, medido el **2026-11-25**
> con el dispositivo `vivero-DDAC03` en línea.
> **Para:** equipo de frontend (y backend, por la parte de retención de datos).
> **Companion:** [`frontend_diseno.md`](frontend_diseno.md) (qué construir) ·
> [`frontend.md`](frontend.md) (referencia técnica).
>
> **Método:** inspección del DOM real (estructura, ARIA, contraste calculado,
> tamaños), captura de pantalla y consulta de `/dashboard/data`. Los números de
> esta página son **medidos**, no estimados.

---

## 1. Veredicto

| Dimensión | Nota | En una frase |
|---|---|---|
| **Simpleza** | 🟡 5/10 | Muestra todo en una sola página sin jerarquía: para ver algo hay que scrollear y leer jerga técnica |
| **Accesibilidad** | 🔴 3/10 | Contraste excelente, pero **sin `<h1>`, sin labels en los inputs, gráficos invisibles para lectores de pantalla y sin anuncios de estado** |
| **Análisis y estadísticas** | 🔴 2/10 | Hay 2 gráficos con ventana fija. **El backend solo devuelve 8,4 horas de datos**: no se puede hacer ninguna estadística real |

**La accesibilidad es lo más barato de arreglar** (días). **El análisis está
bloqueado en el backend** (retención + endpoint de rangos, ticket T7).

---

## 2. Simpleza — qué impide leer rápido

### 2.1 Lo que funciona bien

- **Tarjetas de métricas con el número grande**: 24.7 °C se lee de un vistazo.
- **Badge de estado** "En línea" con color, junto al selector.
- **Agrupación visual** en tarjetas con bordes y fondos distintos.
- **Una sola pantalla** para el estado general: sin navegar para ver lo básico.

### 2.2 Problemas de claridad

| # | Problema | Evidencia | Qué provoca |
|---|---|---|---|
| **S1** | **Jerga técnica a la vista** | `habilitado · off`, `Config: backend`, `IRRIGATION_STOP`, `riego_z1`, `COMMAND_REJECTED` | El operador del vivero no sabe qué significa. Hay que traducir: *"Riego Z1 detenido"*, *"Fuente: programado"* |
| **S2** | **Sin navegación** | página única de **1184 px** con 6 secciones | Todo apilado; para llegar a "Controles" hay que scrollear. No hay forma de ir directo a "Riego" o "Alarmas" |
| **S3** | **La duración se escribe en segundos crudos** | `Duración (s): 600` | Nadie piensa en segundos. Debería ser `10 min` con selector (1/5/10/15/30 min) |
| **S4** | **Las métricas no dicen si están bien** | `Humedad Z1 29.0 %` sin contexto | ¿Es buena? ¿Mala? Falta el umbral, el color de estado y la tendencia (↑↓) |
| **S5** | **Sin indicación de tendencia ni comparación** | solo el valor instantáneo | "Sube" o "baja desde hace 2 h" es lo que permite decidir |
| **S6** | **16 botones de control en desorden** | 4 filas de botones mezclados | Riego, cama, luz, ventana y alarma juntos. Deberían agruparse por sistema y pedir confirmación |
| **S7** | **Sin feedback de los comandos** | no hay zona de `ack` | El operador pulsa y no sabe si el equipo lo aceptó. (Hay un `COMMAND_REJECTED` de `iluminacion` a las 02:39 que nadie pudo ver) |
| **S8** | **Estados vacíos pobres** | "Sin alarmas activa" como texto plano | Debería decir *"Todo en orden · última revisión hace 4 s"* |
| **S9** | **Los gráficos no se pueden leer con precisión** | sin rango seleccionable, sin tooltips visibles, sin eje de umbrales | No se puede responder "¿a qué hora pasó de 30 %?" |
| **S10** | **Los eventos no se pueden filtrar** | lista de los últimos 25, en crudo | No hay forma de ver solo los riegos o solo las alarmas |

---

## 3. Accesibilidad — medición WCAG 2.2

### 3.1 Lo que está bien ✅

| Requisito | Medido | Resultado |
|---|---|---|
| Idioma de la página | `lang="es"` | ✅ |
| Viewport móvil | `<meta name="viewport">` presente | ✅ |
| Contraste texto principal | `#cbd5f` sobre `#020617` = **13.59:1** | ✅ AAA (7:1) |
| Contraste texto secundario | **7.87:1** | ✅ AAA |
| Contraste botones | **13.35:1** | ✅ AAA |
| Nombres accesibles en botones | 19/19 con texto | ✅ |
| Imágenes sin `alt` | 0 (no hay `<img>`) | ✅ |
| Tamaño base de fuente | 16 px | ✅ |

**El contraste no es un problema** — está muy por encima de lo exigido.

### 3.2 Problemas 🔴

| # | Hallazgo | WCAG 2.2 | Sev. | Detalle |
|---|---|---|---|---|
| **A1** | **No existe `<h1>`** | 1.3.1 | 🔴 | La jerarquía arranca en `<h3>`. Un lector de pantalla no puede navegar por encabezados ni entender el título de la página |
| **A2** | **Selector de dispositivo sin label** | 1.3.1, 4.1.2 | 🔴 | `<select>` sin `<label>`, sin `aria-label`, sin `id`. Se anuncia como "combo box, vivero-DDAC03" sin saber qué es |
| **A3** | **Input de duración sin label** | 1.3.1, 3.3.2 | 🔴 | `<input type=number>` sin label ni placeholder identificatorio |
| **A4** | **2 `<canvas>` sin texto alternativo** | 1.1.1 | 🔴 | Los gráficos son **completamente invisibles** para lectores de pantalla. Ni `aria-label`, ni tabla con los datos, ni resumen |
| **A5** | **Sin regiones `aria-live`** | 4.1.3 | 🔴 | Cero elementos `[aria-live]`, `[role=status]` o `[role=alert]`. Cuando llega una alarma o falla un comando, **nadie lo escucha** |
| **A6** | **Cero roles ARIA** | 4.1.2 | 🟠 | `document.querySelectorAll('[role]')` = 0. Sin landmarks (`main`, `nav`, `region`) |
| **A7** | **Sin skip link** | 2.4.1 | 🟠 | No hay "saltar al contenido"; con 1184 px de página el teclado recorre todo |
| **A8** | **Botones de 34 px** | 2.5.8 (mín. 24 px) | 🟠 | Cumple AA pero está por debajo de los **44 px** recomendados para táctil. En celular cuesta apuntar |
| **A9** | **Sin tablas** | 1.3.1 | 🟠 | Cero `<table>`: los datos tabulares (series, eventos, alarmas) no tienen alternativa textual |
| **A10** | **Color como única señal** | 1.4.1 | 🟠 | 363 elementos con color. En las tarjetas de canal y las alarmas, el color distingue estado sin un texto o icono equivalente |

### 3.3 Impacto

Ninguno de estos problemas impide el uso con mouse y vista normal. Pero:
**A1–A5 rompen por completo el uso con lector de pantalla**, y A8/A10
dificultan el uso en celular o con baja visión. Son corregibles en **pocos
días** y no dependen del backend.

---

## 4. Análisis y estadísticas — el bloqueo real

### 4.1 Lo que hay

| Recurso | Estado |
|---|---|
| 2 gráficos de líneas (temperaturas, humedad) | ✅ |
| 9 campos por muestra | ✅ (`ts`, aire T/H, sustrato Z1/Z2 T+%, cama, ventana) |
| Eventos con `code`, `severity`, `channel`, `ts` | ✅ |
| Alarmas activas / latched | ✅ (solo estado actual) |

### 4.2 🔴 El bloqueo: no hay datos suficientes

Medido el 2026-11-25:

```
seriesLen : 240         ← tope fijo
primer    : 2026-11-25T04:09:37
ultimo    : 2026-11-25T12:33:54
ventana   : 8,4 horas    ← y solo del día de hoy
eventosLen: 25           ← tope fijo
```

**240 muestras a 30 s = 2 horas teóricas; en la práctica la ventana visible es
8,4 horas y no cruza días.** Con eso **no se puede hacer ninguna estadística
útil**:

| Análisis | ¿Se puede? | Qué falta |
|---|---|---|
| Promedio/max/mín de la semana | ❌ | histórico > 24 h |
| Cuántos riegos esta semana | ❌ | histórico de eventos > 25 |
| Comparar Z1 vs Z2 en el mes | ❌ | histórico |
| Eficiencia del riego (¿cuánto tarda en recuperarse?) | ❌ | días de datos |
| Frecuencia de alarmas por tipo | ❌ | histórico de alarmas |
| Horas fuera de rango de temperatura | ❌ | histórico |
| Heatmap hora del día vs temperatura | ❌ | días de datos |
| Tendencia y predicción de humedad | ❌ | días de datos |
| Estadística descriptiva de cualquier rango | ❌ | endpoint de rango (T7) |
| Exportar a CSV/Excel/R | ❌ | endpoint de exportación |

**Conclusión:** el frontend no falla por diseño — **el backend no le da
material**. Arreglar el frontend no resuelve nada aquí.

### 4.3 Qué habría que pedir al backend

Extiende el ticket **T7** de [`tickets_backend.md`](tickets_backend.md):

1. **Retención:** guardar telemetría **≥ 90 días** (hoy parece ser horas).
   Con 30 s de muestreo son ~2880 muestras/día → 260 000 filas en 90 días para
   un dispositivo. Conviene **agregación por defecto**: guardar detalle a 30 s
   solo 7 días, y luego promedio cada 5 min (o 15 min) indefinidamente.
2. **`GET /api/telemetry` con `from`/`to`/`interval`** — ya en T7.
3. **`GET /api/stats`** para agregados sin traer el detalle:
   `avg/min/max/count` por métrica y por rango.
4. **Retención de eventos ≥ 1 año** (son pocos: decenas por día) y alarmas
   **sin tope** (son el registro de incidencias).
5. **`GET /api/telemetry/export?format=csv`** para análisis externo.

---

## 5. Catálogo de análisis valiosos (para priorizar)

Ordenado por **valor para el operador del vivero**:

### 🔴 Alta prioridad — decisiones de todos los días

| Análisis | Pregunta que responde |
|---|---|
| **Humedad actual vs umbral con tendencia** | "¿Riego Z1 o espero?" — valor + Δ/h + tiempo estimado hasta el umbral |
| **Riegos ejecutados vs programados** | "¿Se cumple la programación?" — % de horarios respetados |
| **Duración de riego por zona (semana)** | "¿Cuánta agua llevo?" — total de minutos por zona |
| **Temperatura fuera de rango** | "¿Cuánto tiempo estuvo la cama por encima de 35 °C?" |
| **Alarmas por tipo y frecuencia** | "¿Qué se rompe más?" — ranking de condiciones |

### 🟡 Media prioridad — optimización

| Análisis | Pregunta |
|---|---|
| **Recuperación de humedad tras el riego** | "¿Cuánto tarda en subir y cuánto en volver a bajar?" → define el intervalo óptimo |
| **Comparación Z1 vs Z2** | "¿Una zona se seca más rápido que la otra?" → umbrales distintos |
| **Eficacia de la ventilación** | "¿Baja de verdad la humedad cuando se enciende?" |
| **Horas de luz por semana** | "¿Se respeta el fotoperiodo?" |
| **Heatmap hora del día × temperatura** | "¿Cuándo conviene ventilar?" |

### 🟢 Baja prioridad — pulido

| Análisis | Pregunta |
|---|---|
| **Calidad del sensor** | ruido, deriva, valores atípicos por sonda |
| **MTBF de alarmas** | tiempo medio entre fallas por condición |
| **Disponibilidad del equipo** | % de tiempo online, número de reconexiones |
| **Correlación humedad–temperatura** | comportamiento del ambiente |

---

## 6. Recomendaciones priorizadas

### 🔴 Hacer ya — no depende de nadie (≈ 1 semana)

| # | Acción | Cierra |
|---|---|---|
| R1 | Agregar `<h1>` y reordenar la jerarquía (`h1` → `h2` → `h3`) | A1 |
| R2 | `<label>`/`aria-label` en el selector y el input de duración | A2, A3 |
| R3 | `aria-label` + **tabla de datos oculta** en cada `<canvas>` | A4 |
| R4 | Región `aria-live="polite"` para comandos y `role="alert"` para alarmas | A5 |
| R5 | Landmarks (`<main>`, `<nav>`) + skip link | A6, A7 |
| R6 | Botones ≥ 44 px en móvil | A8 |
| R7 | Traducir la jerga: `IRRIGATION_STOP` → *"Riego Z1 detenido"*; `riego_z1` → *"Riego Z1"* | S1 |
| R8 | Duración con selector de minutos, no segundos crudos | S3 |
| R9 | Estado + tendencia en las tarjetas (umbral, ↑↓, color con texto) | S4, S5 |
| R10 | Mostrar el resultado del comando (`ack`) tras cada pulsación | S7 |
| R11 | Agrupar los 16 botones por sistema + confirmación | S6 |
| R12 | Estados vacíos con mensaje útil | S8 |

### 🟠 Requiere backend (bloqueante para el análisis)

| # | Acción | Ticket |
|---|---|---|
| R13 | Retención ≥ 90 días con agregación a 5 min | **T7 (ampliar)** |
| R14 | `GET /api/telemetry` con `from`/`to`/`interval` | **T7** |
| R15 | `GET /api/stats` (avg/min/max/count) | **nuevo** |
| R16 | `GET /api/telemetry/export?format=csv` | **nuevo** |
| R17 | Retención de eventos ≥ 1 año, alarmas sin tope | **T3** |

### 🟡 Requiere backend (ya en la cola)

`T1` (config → permite programar), `T2` (histórico de alarmas), `T4/T8`
(ack), `T5` (selector con estado), `T6` (diagnóstico).

---

## 7. Respuesta directa a las dos preguntas

### ¿Sirve para mostrar la información de forma simple y accesible?

**Simple, parcialmente.** Lo básico se ve bien (números grandes, tarjetas,
badge de online), pero **está apilado en una sola página sin navegación, con
jerga técnica sin traducir y sin decir si un valor es bueno o malo**. Un
operador puede mirar, no puede decidir.

**Accesible, no.** El contraste está excelente (AAA en todo), el idioma y el
viewport están bien, y todos los botones tienen nombre. Pero **no hay `<h1>`,
los dos inputs no tienen label, los gráficos son invisibles para lectores de
pantalla y no hay ninguna región viva para anunciar cambios**. Con lector de
pantalla el portal es prácticamente inutilizable.

### ¿Permite generar análisis y estadísticas?

**No, y el problema no está en el frontend.** Hay 2 gráficos con ventana fija
y **el backend devuelve 8,4 horas de datos con tope de 240 muestras y 25
eventos**. Con eso no se puede calcular ni un promedio semanal.

**Para que el frontend pueda analizar, primero el backend debe retener.**
Hasta que exista `T7` + retención, cualquier pantalla de estadísticas sería
decorativa. Recomendación: **no invertir en gráficos avanzados todavía**;
primero R1–R12 (accesibilidad y simpleza, que no dependen de nadie) y después
desbloquear la retención.
