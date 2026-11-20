# Material para el equipo de backend y frontend

**Empezá por acá.** Este folder concentra todo lo que necesitás para desarrollar
la API, el servidor WebSocket y la aplicación web, sin tener que leer el
firmware.

| Documento | Para quién | Qué contiene |
|---|---|---|
| [`backend.md`](backend.md) | Backend | alcance, HTTP, servidor WS, modelo de datos, catálogos, pruebas |
| [`frontend.md`](frontend.md) | Frontend | **informe completo**: API real, pantallas, datos, control de actuadores, programación de riego, umbrales, alarmas, huecos de API y casos de prueba |
| [`../02_protocolo_ws.md`](../02_protocolo_ws.md) | **Ambos** | **el contrato: fuente de verdad** |

---

## 1. Qué es esto, en dos párrafos

Hay un **vivero** con bombas de riego, una cama caliente, luminaria, un
extractor, una ventana motorizada y una alarma. Un **ESP32** mide temperatura,
humedad de aire y de sustrato, y enciende esos 8 aparatos según horarios,
umbrales y reglas de seguridad.

El ESP32 funciona **solo** (riega y ventila aunque no haya red). Cuando hay
conexión, se conecta por WebSocket a un backend y manda telemetría, recibe
configuración y acepta comandos puntuales. **Vuestro trabajo es ese backend y
la web que lo usa.**

---

## 2. Qué ya existe y qué falta

| | Estado |
|---|---|
| Firmware ESP32 | ✅ completo (`src/`, versión `0.1.0-m6`) |
| Portal cautivo del dispositivo | ✅ completo. **No es trabajo del frontend**: es HTML servido por el propio ESP32 |
| Contrato del protocolo | ✅ [`../02_protocolo_ws.md`](../02_protocolo_ws.md) |
| Backend falso de prueba | ✅ [`../tools/fake_backend.py`](../tools/fake_backend.py) |
| **Backend real** (HTTP + WS + MariaDB) | ⬜ **por construir** |
| **Frontend web** | ⬜ **por construir** |

---

## 3. Orden de lectura sugerido

1. [`../00_vision_y_alcance.md`](../00_vision_y_alcance.md) — alcance de v0.1
   y qué queda explícitamente afuera (10 min).
2. **[`../02_protocolo_ws.md`](../02_protocolo_ws.md)** — el contrato. Es
   largo; léelo entero una vez. Es la única fuente de verdad de los mensajes.
3. Tu documento: [`backend.md`](backend.md) o [`frontend.md`](frontend.md).
4. [`../03_reglas_y_plan.md`](../03_reglas_y_plan.md) §14 (pruebas) — para
   saber cómo se valida cada cosa.

**No hace falta** leer `src/` para empezar. Solo si necesitás confirmar cómo
se comporta el firmware ante un caso borde.

---

## 4. Cómo probar

### 4.1 El backend falso como referencia

`tools/fake_backend.py` es una implementación **ejecutable** del contrato. Sirve
para tres cosas:

- **Validar el firmware** sin tener el backend real (es lo que se usa en banco).
- **Referencia de implementación** para el equipo de backend: qué responde,
  en qué orden, qué valida.
- **Caso de prueba del protocolo**: `--selftest` verifica claim 403/200, cierre
  4401, `welcome`/`config`, telemetría, `state`, `event`, `command`/`ack`,
  `ping`/`pong` y `request`.

```powershell
python -m pip install "websockets>=14"
python tools\fake_backend.py --selftest
python tools\fake_backend.py --http-port 8080 --ws-port 8081
```

Requisitos: **Python 3.10+** y **`websockets>=14`**.

### 4.2 Lo que ve el backend falso

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

En el portal del dispositivo, `backend_url` = `http://<IP-de-la-PC>:8080` y
`claim_code` = `VIVERO-01`.

---

## 5. Glosario

| Término | Significado |
|---|---|
| **Canal** | uno de los 8 relés fijos. Nombre corto: `riego_z1`, `riego_z2`, `aspersion`, `cama_caliente`, `iluminacion`, `ventilacion`, `ventana`, `alarma` |
| **Zona** | área de riego. Hay 2 de sustrato (`z1`, `z2`) y 1 de aspersión |
| **`NORMAL`** | modo normal: automatización activa, WiFi STA |
| **`PORTAL`** | AP cautivo para configurar. **La automatización no se suspende** |
| **`MANTENIMIENTO`** | relés OFF, automatización y botones suspendidos. Se entra solo desde el portal con confirmación |
| **Perfil local** | configuración guardada en el dispositivo, editable desde el portal |
| **Fuente de config** | `backend` (por defecto, manda el servidor) o `local` (override explícito del operador) |
| **Latch** | alarma que queda "pegada" hasta reset manual, aunque la condición desaparezca |
| **Silencio** | apaga el relé de alarma 10 min **sin** borrar condiciones |
| **`claim_code`** | código de un solo uso que da de alta el dispositivo en el backend |
| **Interlock** | un solo riego a la vez entre Z1, Z2 y aspersión |
| **`max_run_s`** | corte duro de duración de riego; al cortar dispara `irrigation_timeout` |

---

## 6. Convenciones del proyecto

- **Idioma:** los documentos y los rótulos de la UI en **español**. Los
  identificadores de mensaje y de campo del protocolo en **inglés
  snake_case**.
- **Hora:** todos los timestamps van en **UTC ISO 8601** (`...Z`). Los horarios
  se evalúan en **hora local** (offset por defecto `UTC-3`, Argentina sin DST).
- **Unidades:** temperaturas en °C, humedad en %, duraciones en segundos
  (`*_s`), duraciones internas en milisegundos (`*_ms`).
- **Versionado:** el contrato usa `"v": 1` en cada mensaje. Cambios
  incompatibles incrementan ese número.
- **Una fuente de verdad:** si este folder y `02_protocolo_ws.md` dicen cosas
  distintas, **manda `02`** y avisale al equipo de firmware.

---

## 7. Preguntas abiertas (cerrar con el equipo de firmware)

Están listadas en detalle en [`../02_protocolo_ws.md`](../02_protocolo_ws.md)
§15. Las que bloquean arrancar:

| # | Tema | Quién decide |
|---|---|---|
| 1 | Endpoint exacto de alta (`POST /api/v1/devices/claim`) y si el servidor WS soporta **headers** en el upgrade | backend confirma |
| 2 | **Certificado TLS**: el firmware ya conecta por `wss://` con `setInsecure()`; falta decidir validación o pinning | backend |
| 3 | Frecuencia de telemetría (30 s por defecto, ajustable con `welcome.telemetry_s`) | backend |
| 4 | Retención de datos en MariaDB | backend |
| 5 | Autorización de usuarios de la web | backend |

Y una que **no depende de vos** pero conviene conocer: el actuator de ventana
usa un **pulso de 5 s** y tiene **un solo sensor de cerrada** (no distingue
"abierta" de "entreabierta"). El datasheet del controlador sigue pendiente.
