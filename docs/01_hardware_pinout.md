# Vivero IoT — Hardware y pinout (v0.1)

**Placa:** ESP32 DevKit V1, 30 pines (confirmar WROOM vs WROVER).
**Lógica:** 3.3 V.
**Relés:** módulo opto-acoplado de 8 canales, **low-level trigger** confirmado en la base.
**Estado:** propuesta cerrada para v1. Queda pendiente el datasheet exacto del
controlador de ventana; el segundo sensor de ventana se evalúa para v2.
**Hoja de armado:** [`04_conexionado_y_componentes.md`](04_conexionado_y_componentes.md)
y diagrama visual [`diagrama_conexiones.svg`](diagrama_conexiones.svg).

## 1. Tabla de pines

| GPIO | Función | Dirección | Notas |
|---|---|---|---|
| 21 | I2C SDA | Bidireccional | OLED + RTC DS3231 |
| 22 | I2C SCL | Bidireccional | OLED + RTC DS3231 |
| 4 | DHT21 DATA | Bidireccional | Pull-up 10 kΩ a 3.3 V |
| 12 | Alimentación conmutada sensores humedad | Salida | XY-MOS; pull-down 10 kΩ; bajo al arrancar |
| 13 | Sensor de ventana cerrada | Entrada | Pull-up 10 kΩ; switch a GND |
| 16 | DS18B20 Z1 | Bidireccional | Pull-up 4.7 kΩ |
| 32 | DS18B20 Z2 | Bidireccional | Pull-up 4.7 kΩ |
| 33 | DS18B20 cama caliente | Bidireccional | Pull-up 4.7 kΩ |
| 34 | Humedad sustrato Z1 | ADC1, solo entrada | Calibración dry/wet |
| 35 | Humedad sustrato Z2 | ADC1, solo entrada | Calibración dry/wet |
| 26 | Relé 1 — Riego sustrato Z1 | Salida | Low-level |
| 27 | Relé 2 — Riego sustrato Z2 | Salida | Low-level |
| 18 | Relé 3 — Riego aspersión | Salida | Low-level |
| 14 | Relé 4 — Cama caliente | Salida | Low-level |
| 19 | Relé 5 — Iluminación | Salida | Low-level |
| 17 | Relé 6 — Ventilación forzada | Salida | Low-level |
| 23 | Relé 7 — Ventana | Salida | Low-level; pulso de 5 s |
| 5 | Relé 8 — Alarma | Salida | Low-level; pull-up 10 kΩ obligatorio (§3) |
| 25 | Botón Selector | Entrada | `INPUT_PULLUP` interno |
| 36 | Botón Confirmar | Entrada | Pull-up externo 10 kΩ |
| 39 | Botón Configuración | Entrada | Pull-up externo 10 kΩ |

**Reservados, no usar:** GPIO0 (boot), GPIO1 y GPIO3 (UART), GPIO6-11 (flash).
**Libres:** GPIO2 y GPIO15, ambos strapping. No se usan en v1.
**GPIO12:** strapping (MTDI); debe estar **bajo** al arrancar. El pull-down externo
de 10 kΩ lo garantiza. Se usa como salida de alimentación conmutada, activa en
alto después del boot.
**GPIO13:** entrada del sensor de ventana; no es strapping.

## 2. Presupuesto de pines

Con un pin por DS18B20 y sensor de ventana:

- Usados: 8 relés + 3 botones + 2 I2C + 2 ADC + 1 DHT + 1 alimentación conmutada
  + 3 DS18B20 + 1 sensor de ventana = **21**.
- Libres: **GPIO2 y GPIO15**, ambos strapping. GPIO0 queda reservado para boot.
- Para el segundo sensor de ventana de v2 no hay pin no-strapping: opciones son
  GPIO15 con interfaz boot-safe, un expansor I2C (PCF8574) o liberar otra función.
  **Pendiente para v2.**

Con bus OneWire compartido se liberarían GPIO32/33, pero el usuario eligió un pin
por sonda para simplificar el cableado y evitar el mapeo por dirección.

## 3. Conexionado de relés

- IN1-IN8 → GPIO26, 27, 18, 14, 19, 17, 23, 5.
- **Pull-up externo de 10 kΩ a 3.3 V en cada IN** (8 resistencias). Garantiza relé
  apagado mientras el ESP32 arranca.
- **JD-VCC** → +5 V de fuente externa.
- **VCC** → mismo +5 V (jumper JD-VCC↔VCC puesto).
- **GND** del módulo, GND de la fuente externa y GND del ESP32 **unidos**.
- La entrada IN8 ya no se puentea a 3.3 V: ahora va a GPIO5.
- **Nota GPIO5:** es un pin de strapping pero **no condiciona el arranque**. Los
  strapping que sí lo hacen en ESP32 son GPIO0, GPIO2, GPIO12 (MTDI) y GPIO15
  (MTDO), según la documentación de Espressif (*esptool → Boot Mode Selection*).
  El pull-up de 10 kΩ en IN8 sigue siendo **obligatorio**, por otra razón: con
  trigger *low-level* el relé se activa con IN en bajo, y durante el arranque los
  GPIO quedan en alta impedancia hasta que corre `relayInit()`. Sin el pull-up,
  el canal queda en estado indefinido en los primeros segundos de cada arranque.
  Verificar en placa antes de conectar cargas.
- **Relé 7 (ventana):** solo se usa como **pulso de 5 s** hacia el controlador.
  No debe quedar energizado de forma continua.

## 4. Sensores

### 4.1 DHT21

- DATA → GPIO4.
- Pull-up 10 kΩ entre DATA y 3.3 V.
- VCC → 3.3 V, GND → GND.

### 4.2 Humedad de sustrato Z1/Z2

- AOUT Z1 → GPIO34; AOUT Z2 → GPIO35.
- VCC de ambos → salida conmutada del XY-MOS (no directo a 3.3 V).
- GND → GND.
- XY-MOS: VIN+ → 3.3 V; VIN− → GND; OUT+ → VCC sensores; OUT− → GND sensores;
  TRIG → **GPIO12**; GND de control → GND.
- El XY-MOS conmuta masa: GPIO12 alto = sensores energizados; bajo = apagados.
- **GPIO12 es strapping (MTDI):** debe estar bajo al arrancar. Agregar pull-down
  externo de 10 kΩ entre GPIO12 y GND. Antes de `pinMode`, dejar el pin en bajo.
- La conmutación reduce la corrosión de las sondas: energizar solo durante la
  lectura (25 ms de estabilización + 12 muestras) y apagar después.
- Calibración por zona: `dry_raw` y `wet_raw` desde el portal, sin asumir cuál es
  mayor. Lectura promedio de 12 muestras tras 25 ms de estabilización.

### 4.3 DS18B20 (Z1, Z2 y cama caliente)

**Decisión: un pin por sonda.**

- DS18B20 Z1 → GPIO16; Z2 → GPIO32; cama → GPIO33.
- Cada sonda con su pull-up de 4.7 kΩ entre DATA y 3.3 V.
- VCC → 3.3 V; GND → GND.
- No hay bus compartido ni mapeo por dirección: el rol queda fijado por el pin.
- GPIO32/33 no son input-only; se usan como OneWire bidireccional sin problema.
- Ventaja: un sensor flojo en un pin no afecta a los otros. Desventaja: usa un pin
  por sonda y deja sin pines no-strapping para el segundo sensor de ventana.

### 4.4 Estado de ventana

**v1: un solo sensor de cerrada.**

- “Cerrada” → GPIO13; switch/reed a GND, pull-up externo de 10 kΩ a 3.3 V.
- Cerrada = LOW; no cerrada = HIGH.
- Debounce de 50 ms.
- Estados: `CLOSED` (sensor activo), `OPEN` (sensor inactivo y sin movimiento),
  `MOVING` (pulso o viaje en curso), `FAULT` (el sensor no cambió tras el pulso).
- El sensor solo confirma cerrada. La posición “abierta” se deduce; el controlador
  del motor maneja el límite de apertura.
- **Limitación:** un sensor trabado en “no cerrada” puede hacer que un cierre
  genere una apertura. Mitigación: probar el sensor en el portal antes de habilitar
  la automatización; si no cambia tras un pulso, pasar a `FAULT` y no repetir.
- **v2:** segundo sensor (abierta) en GPIO15 con interfaz boot-safe o expansor I2C
  PCF8574. No se implementa en v1.

## 5. Actuador de ventana (pulso toggle)

**Confirmado:** el canal 7 no alimenta el motor directamente. Entrega un **pulso de
5 s** a un controlador propio, que **alterna** el sentido: un pulso abre, el
siguiente cierra, etc. El controlador maneja los límites de recorrido; el ESP32
solo da la orden y lee el sensor de cerrada.

- Relé 7: contacto seco (COM/NO) a la entrada del controlador.
- Secuencia: energizar 5000 ms → desenergizar → esperar transición del sensor.
- No dejar el relé cerrado: el controlador puede interpretar un pulso largo como
  otra orden o quedar en estado inválido.
- Mínimo 60 s entre pulsos (ajustar al datasheet del controlador).
- Nunca pulsar en `MOVING`, `FAULT` o estado desconocido: un toggle en el momento
  equivocado mueve la ventana al revés.
- Para abrir se exige `CLOSED`; para cerrar se exige `OPEN` (sensor inactivo).
- Si el controlador tiene entrada de 12/24 V, el contacto del relé conmuta esa
  tensión; verificar aislamiento y que el módulo soporte la tensión/corriente.
- **Pendiente:** modelo/datasheet del controlador para confirmar polaridad, tiempo
  mínimo entre pulsos y comportamiento ante un pulso durante el movimiento.

## 6. Alimentación

| Rail | Alimenta |
|---|---|
| 5 V externo, 2 A | Módulo de relés (JD-VCC y VCC) |
| 3.3 V del ESP32 | OLED, RTC, DHT21, DS18B20, VIN+ del XY-MOS |
| GND común | ESP32 + fuente 5 V + módulo de relés + sensores |

- Consumo estimado del módulo: 70-90 mA por relé activo; 8 relés ≈ 720 mA. Fuente
  de 5 V/2 A para margen y pico de bobinas.
- Cargas de 220 V (cama caliente, iluminación, ventilación, bomba): usar contactor
  o relé de potencia adecuado, fusible, puesta a tierra y gabinete. No conmutar
  220 V directo con el módulo genérico sin verificar tensión/corriente de contacto.
- Para cargas inductivas (motores, bombas), agregar snubber/varistor.

## 7. Checklist de armado

- [ ] Confirmar WROOM vs WROVER (afecta GPIO16/17).
- [ ] Confirmar módulo de relés de 8 canales y polaridad low-level.
- [ ] Verificar que los 8 relés quedan OFF al arrancar (pull-ups en IN1-IN8).
- [ ] Verificar GPIO12 bajo al arrancar (pull-down 10 kΩ) y alto solo al energizar sensores.
- [ ] Medir 3.3 V en reposo en GPIO36 y GPIO39 (pull-ups externos de los botones).
- [ ] Confirmar controlador de ventana, polaridad del pulso y tiempo mínimo entre pulsos.
- [ ] Verificar que el relé 7 se energice 5 s y se suelte; nunca continuo.
- [ ] Verificar un pin por DS18B20 (16, 32, 33) con pull-up de 4.7 kΩ en cada uno.
- [ ] Verificar sensor de cerrada en GPIO13; prever el segundo sensor para v2.
- [ ] Verificar todos los relés OFF al energizar, antes de conectar cargas.
- [ ] Probar una zona completa en banco (sensor + relé + carga resistiva).
- [ ] Revisar fusible, tierra y gabinete antes de conectar 220 V.
