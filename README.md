# Vivero IoT

Firmware para **ESP32** que monitorea variables ambientales de un vivero y
controla **8 actuadores**, con operación **100 % local** (funciona sin
internet) y telemetría/comandos hacia un backend por **WebSocket**.

| | |
|---|---|
| Placa | ESP32 DevKit V1, 30 pines |
| Framework | Arduino vía PlatformIO |
| Versión actual | `0.1.0-m6` |
| Estado | Compila limpio y el backend falso pasa su `--selftest`. **Pendiente de validación en banco** (casos T18–T31) |

---

## Qué hace

- **8 canales fijos:** riego Z1, riego Z2, aspersión, cama caliente,
  iluminación, ventilación, ventana y alarma. La función y el GPIO **no se
  reconfiguran** desde ningún lado: el portal solo los habilita/deshabilita y
  los prueba.
- **7 sensores:** DHT21 (aire), 2 humedades de sustrato, 3 temperaturas
  DS18B20 (Z1, Z2 y cama caliente) y un sensor de ventana cerrada.
- **3 modos:** `NORMAL`, `PORTAL` (AP cautivo — **la automatización sigue
  activa**) y `MANTENIMIENTO` (relés OFF, automatización suspendida).
- **Sin backend:** sigue regando, ventilando y controlando clima con la última
  configuración válida cacheada.
- **Con backend:** telemetría, eventos, comandos y configuración por
  WebSocket, con cola offline.

---

## Compilar

```powershell
cd "C:\Users\javier\Desktop\OPENCODE PROYECTOS\vivero_iot"
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run
```

`pio` no está en el PATH de este entorno; usar la ruta de arriba. Un build
completo tarda ~4 min; los incrementales ~2 min.

---

## Mapa del repositorio

| Ruta | Qué hay |
|---|---|
| `src/` | firmware (27 módulos `.cpp`) |
| `docs/` | especificaciones del proyecto |
| **`docs/equipo/`** | **material de trabajo para backend y frontend** |
| `tools/fake_backend.py` | backend de prueba (claim + WS) |
| `platformio.ini` | entorno y librerías con versión fija |
| `.gitignore` | `.pio/`, `__pycache__`, secretos |

---

## Documentos

| Documento | Para qué sirve |
|---|---|
| `docs/00_vision_y_alcance.md` | qué entra y qué queda fuera en v0.1 |
| `docs/01_hardware_pinout.md` | pines, hardware y restricciones |
| **`docs/02_protocolo_ws.md`** | **contrato WebSocket — fuente de verdad** |
| `docs/03_reglas_y_plan.md` | reglas de control, NVS, plan por hitos y casos de prueba |
| `docs/04_conexionado_y_componentes.md` | hoja de armado, BOM y verificaciones |
| `docs/diagrama_conexiones.svg` | diagrama general de conexionado |
| `docs/diagrama_sensor_ventana.svg` | conexionado del sensor de ventana |
| **`docs/equipo/`** | **onboarding y requerimientos para el equipo** |

---

## Quién hace qué

| Rol | Trabajo | Empezar en |
|---|---|---|
| **Firmware** (este repo) | ESP32, portal cautivo, reglas, alarmas, cliente WS | `src/` |
| **Backend** | API HTTP + servidor WS + MariaDB | [`docs/equipo/backend.md`](docs/equipo/backend.md) |
| **Frontend** | aplicación web sobre la API | [`docs/equipo/frontend.md`](docs/equipo/frontend.md) |

---

## Probar sin hardware

`tools/fake_backend.py` implementa el mínimo del contrato (claim HTTP +
WebSocket) y sirve para validar el firmware y para que el equipo de backend
tenga una referencia ejecutable del protocolo.

```powershell
python -m pip install "websockets>=14"
python tools\fake_backend.py --http-port 8080 --ws-port 8081
python tools\fake_backend.py --selftest    # valida el protocolo sin firmware
```

---

## Estado de los hitos

| Hito | Estado |
|---|---|
| M0 Base y esqueleto | ✅ |
| M1 8 relés y habilitación | ✅ |
| M2 Sensores | ✅ |
| M3 Reglas y control local | ✅ |
| M4 Alarmas | ✅ |
| M5 Portal y modos | ✅ |
| M6 WebSocket | ✅ código completo, **falta validación en banco** |
| M7 Integración y puesta en marcha | ⬜ pendiente |

**Pendiente bloqueante de hardware:** datasheet del controlador de ventana
(polaridad del pulso, tiempo mínimo entre pulsos), WROOM vs WROVER, y las
cargas reales del gabinete.
