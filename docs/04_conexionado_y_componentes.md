# Vivero IoT — Conexionado y lista de componentes (v0.1)

**Placa:** ESP32 DevKit V1, 30 pines (confirmar WROOM vs WROVER).
**Lógica:** 3.3 V. **Relés:** módulo opto-acoplado de 8 canales, low-level trigger.
**Diagrama visual:** [`diagrama_conexiones.svg`](diagrama_conexiones.svg) (abrir en el navegador; se puede imprimir en A3).
**Pinout de referencia:** [`01_hardware_pinout.md`](01_hardware_pinout.md).

Este documento es la hoja de armado: qué comprar, qué va a cada bornera y qué
verificar antes de energizar. Las cargas de 220 V siguen **pendientes de
definición** (§2.2 y §9): el listado de potencia es orientativo hasta conocer
bombas, resistencias y extractores reales.

---

## 1. Cómo leer el conexionado

- **GPIO nn** = pin del ESP32. En la placa buscarlo por la serigrafía; el orden
  físico de los pines cambia entre clones, no guiarse por la posición.
- **3V3** = riel de 3.3 V que sale del ESP32. **5V** = riel de la fuente externa.
- **GND** = masa común de todo el conjunto (fuente, ESP32, relés y sensores).
- Todos los relés son **activos en bajo**: GPIO en 0 V = relé cerrado.
- Los pines GPIO34, 35, 36 y 39 son **solo entrada** y **no tienen pull-up
  interno**: si no se usan como ADC o como entrada con pull-up externo, quedan
  flotando.

---

## 2. Lista de componentes

### 2.1 Electrónica de control (definida)

| # | Componente | Cant. | Especificación | Notas |
|---|---|---|---|---|
| 1 | ESP32 DevKit V1 | 1 | 30 pines, ESP32-WROOM-32, 4 MB flash, USB micro | **Confirmar WROOM**: en WROVER, GPIO16/17 los usa la PSRAM |
| 2 | Módulo de relés | 1 | 8 canales, 5 V, optoacoplado, **low-level trigger**, relé SRD-05VDC-SL-C (10 A/250 VAC) | Con jumper JD-VCC; bornera IN1-IN8 |
| 3 | Fuente conmutada | 1 | 100-240 VAC → 5 VDC, **2 A mínimo** (3 A recomendado) | Alimenta ESP32 + módulo de relés |
| 4 | Módulo XY-MOS | 1 | Interruptor MOSFET de potencia, disparo 3.3 V, **high-side** | Alimentación conmutada de los sensores de humedad |
| 5 | Módulo RTC | 1 | DS3231, I2C, con batería CR2032 | Dirección I2C `0x57` |
| 6 | Display OLED | 1 | SSD1306 0.96", 128×64, I2C, 3.3 V | Dirección I2C `0x3C` |
| 7 | DHT21 / AM2301 | 1 | Temperatura y humedad de aire, 3.3 V | DHT22/AM2302 sirve como reemplazo (`AIR_SENSOR_MODEL 2`) |
| 8 | Sensor de humedad de sustrato | 2 | Analógico 0-3 V, **capacitivo recomendado** | Uno por zona (Z1, Z2). Los resistivos se corroen |
| 9 | Sonda DS18B20 | 3 (+1 repuesto) | Estanca, acero inoxidable, cable 1 m, 3 hilos | Z1, Z2 y cama caliente. **No usar modo parásito** |
| 10 | Sensor magnético de ventana | 1 | Reed, contacto a GND | v1: solo confirma "cerrada" |
| 11 | Pulsadores | 3 | 12 mm, NA, montaje en panel | Selector, Confirmar, Configuración |
| 12 | Resistencias 10 kΩ | 13 | 1/4 W, 5 % | 8 relés + DHT21 + GPIO12 + ventana + 2 botones |
| 13 | Resistencias 4.7 kΩ | 5 | 1/4 W, 5 % | 3 DS18B20 + 2 I2C (solo si los módulos no las traen) |
| 14 | Capacitores 100 nF | 4 | Cerámicos 50 V | Desacople opcional en DHT21 y sensores de humedad |
| 15 | Borneras | ~12 | Paso 5.08 mm, 2 y 3 vías | Una por sensor y por salida de relé |
| 16 | Placa perforada o PCB | 1 | 10×15 cm mínimo | Para el cableado de señales y resistencias |

### 2.2 Potencia y gabinete (según carga real)

Las cargas finales no están definidas. Hasta cerrarlas, esto es una base:

| # | Componente | Cant. | Especificación | Notas |
|---|---|---|---|---|
| 17 | Contactor | 2-4 | Bobina 220 V, contactos según carga | Bomba y cargas inductivas; el módulo de relés solo manda la bobina |
| 18 | Breaker 2P | 1 | 10 A curva C | Entrada del tablero |
| 19 | Diferencial 2P | 1 | 30 mA | Obligatorio si el tablero alimenta tomas o equipo a la intemperie |
| 20 | Portafusible + fusible | 1 | 2 A, 5×20 mm | Protección del riel de 5 V |
| 21 | Varistor | 1 | 275 V (14D471K) | Protección de picos en la entrada de 220 V |
| 22 | Snubber RC | 1 por carga inductiva | 100 Ω + 0.1 µF X2 | Contactos de contactores y motores |
| 23 | Gabinete | 1 | IP65, con riel DIN | Con prensacables y puesta a tierra |
| 24 | Borneras de riel DIN | — | 4 mm² | Distribución de 220 V, 5 V y tierra |
| 25 | Cable 220 V | — | 1.5 mm² (10 A) o 2.5 mm² (16 A), 3 conductores | Fase, neutro y tierra |
| 26 | Cable de tierra | — | 2.5 mm² verde/amarillo | Desde el gabinete a la jabalina |
| 27 | Cable de señales | — | 0.22-0.5 mm² | Sensores y botones |
| 28 | Cable 5 V | — | 0.75 mm² | Hasta 720 mA con los 8 relés activos |

### 2.3 Consumibles

Termocontraíble, terminales de horquilla, precintos, canaleta, etiquetas de
bornera, tornillería M3 y separadores para la placa.

---

## 3. Esquema general

```text
        220 VAC  L / N / PE
                │
       [Breaker 2P 10 A]
       [Diferencial 30 mA]
       [Fusible 2 A] ── solo rama de 5 V
                │
       [Fuente 5 V / 2 A]
           │         │
        +5 V       GND ─────────────────────────────┐
           │                                        │
           ├──────────────► Módulo relés (VCC/JD-VCC)│
           │                    │                   │
           │                8 contactos ──► cargas  │
           │                                        │
        ESP32 DevKit V1                            │
        │  3V3 ──► OLED, RTC, DHT21, DS18B20, XY-MOS│
        │  GPIO ──► relés (IN1-IN8)                 │
        │  GPIO ──► sensores y botones              │
        │                                        │
        └──────────── GND común ─────────────────┘
```

Regla de oro: **una sola masa**. La fuente de 5 V, el ESP32, el módulo de relés
y todos los sensores comparten GND. Si el módulo de relés usa JD-VCC separado
con el jumper quitado, la masa sigue siendo común.

---

## 4. Alimentación

### 4.1 Rieles

| Riel | Origen | Alimenta |
|---|---|---|
| **5 V** | Fuente externa 5 V / 2 A | ESP32 (pin 5V/VIN), módulo de relés (VCC y JD-VCC) |
| **3V3** | Regulador del ESP32 | OLED, DS3231, DHT21, 3 × DS18B20, VIN+ del XY-MOS, pull-ups |
| **GND** | Fuente externa | Todo el conjunto |

### 4.2 Consumo estimado

| Consumidor | 5 V | 3.3 V |
|---|---|---|
| ESP32 (WiFi transmitiendo) | 250 mA | — |
| Módulo de relés, 8 bobinas activas | 720 mA | — |
| OLED | — | 20 mA |
| DS3231 | — | 0.2 mA |
| DHT21 | — | 1.5 mA |
| 3 × DS18B20 | — | 4.5 mA |
| 2 × sensor de humedad | — | 20 mA |
| **Total** | **≈ 1.0 A** | **≈ 46 mA** |

La fuente de 2 A queda al 50 % con todo encendido. Si se prevé que coincidan
riego, iluminación, ventilación y cama, usar **3 A**. El regulador del ESP32
solo entrega los 46 mA de 3.3 V, muy por debajo de su límite.

### 4.3 Secciones y protecciones

- Entrada y cargas de 220 V: 1.5 mm² hasta 10 A; 2.5 mm² si se supera.
- Tierra: 2.5 mm² verde/amarillo, continua desde el gabinete a la jabalina.
- 5 V: 0.75 mm² por la corriente de las bobinas.
- Señales: 0.22-0.5 mm²; cables cortos y lejos de los conductores de 220 V.
- DS18B20 con cable apantallado si el tendido supera 3 m; malla a GND de un
  solo lado.

### 4.4 Alimentar el ESP32

- 5 V al pin **5V / VIN** del DevKit (no al pin 3V3).
- No conectar el USB y la fuente externa al mismo tiempo si la placa no tiene
  diodo de protección entre ambas entradas. Para programar, desconectar la
  fuente o alimentar por USB.
- El pin 3V3 del ESP32 **no** se usa para alimentar relés ni cargas: solo
  sensores y módulos de baja corriente.

---

## 5. Conexionado por bloque

### 5.1 Módulo de relés

| Canal | Función | GPIO | Entrada | Salida (COM/NO) | Carga |
|---|---|---|---|---|---|
| 1 | Riego sustrato Z1 | 26 | IN1 | COM1 / NO1 | Electroválvula Z1 |
| 2 | Riego sustrato Z2 | 27 | IN2 | COM2 / NO2 | Electroválvula Z2 |
| 3 | Riego aspersión | 18 | IN3 | COM3 / NO3 | Electroválvula de aspersión |
| 4 | Cama caliente | 14 | IN4 | COM4 / NO4 | Manta o cable calefactor |
| 5 | Iluminación | 19 | IN5 | COM5 / NO5 | Luminaria |
| 6 | Ventilación forzada | 17 | IN6 | COM6 / NO6 | Extractor |
| 7 | Ventana | 23 | IN7 | COM7 / NO7 | Entrada del controlador (pulso 5 s) |
| 8 | Alarma | 5 | IN8 | COM8 / NO8 | Baliza o sirena |

Alimentación del módulo:

| Terminal | Conexión |
|---|---|
| VCC | +5 V |
| JD-VCC | +5 V (jumper JD-VCC↔VCC **puesto**) |
| GND | GND común |

- **Pull-up de 10 kΩ de cada IN a 3V3** (8 resistencias). Mantiene el relé
  apagado mientras el ESP32 arranca con los pines en alta impedancia.
- **Relé 7 (ventana):** solo pulso de 5 s. No dejarlo energizado.
- **Relé 8 (alarma):** GPIO5 es strapping pero **no condiciona el arranque**; el
  pull-up de 10 kΩ es lo que garantiza que el canal arranque apagado, porque los
  GPIO están en alta impedancia hasta `relayInit()`.
- **Verificación obligatoria:** con el módulo alimentado a 5 V y el ESP32
  apagado, medir tensión en cada IN. Si supera 3.4 V, la entrada del módulo
  está referenciada a 5 V; en ese caso quitar el jumper y alimentar **VCC con
  3.3 V** dejando JD-VCC en 5 V, o intercalar una resistencia de 1 kΩ en serie
  con cada IN. No conectar los GPIO así sin resolverlo.

### 5.2 Salidas a cargas

- Usar **COM + NO**: la carga queda sin tensión cuando el relé está en reposo.
- Contacto del módulo: 10 A / 250 VAC **resistivos**. Para bomba, extractor o
  cualquier motor, mandar la **bobina de un contactor**, no la carga.
- Cada carga de 220 V con fusible o breaker propio y, si es inductiva, snubber
  RC y varistor.
- **Ventana:** contacto seco a la entrada del controlador. Confirmar con el
  datasheet si esa entrada es de 12/24 V y si el módulo soporta esa tensión.
- **Alarma:** baliza 12/24 V con fuente propia o sirena de 220 V vía contactor.

### 5.3 Sensores

| Sensor | VCC | GND | Señal | Pull-up |
|---|---|---|---|---|
| DHT21 (aire) | 3V3 | GND | GPIO4 (DATA) | 10 kΩ a 3V3 |
| Humedad Z1 | OUT+ del XY-MOS | GND | GPIO34 (AOUT) | — |
| Humedad Z2 | OUT+ del XY-MOS | GND | GPIO35 (AOUT) | — |
| DS18B20 Z1 | 3V3 | GND | GPIO16 (DQ) | 4.7 kΩ a 3V3 |
| DS18B20 Z2 | 3V3 | GND | GPIO32 (DQ) | 4.7 kΩ a 3V3 |
| DS18B20 cama | 3V3 | GND | GPIO33 (DQ) | 4.7 kΩ a 3V3 |
| Ventana cerrada | — | GND (un contacto) | GPIO13 (otro contacto) | 10 kΩ a 3V3 |

- **DHT21:** cable de hasta 1 m. Respetar 2 s entre lecturas (el firmware lee
  cada 30 s).
- **DS18B20:** tres hilos (VCC, GND, DQ). **No** usar alimentación parásita.
- **Ventana:** cerrada = nivel **bajo**. Si se usa un reed normalmente cerrado,
  el sentido se invierte y hay que cambiar `WINDOW_CLOSED_LEVEL` en `config.h`.
- **Humedad:** el ADC espera 0-3.3 V. Verificar que el módulo no entregue 5 V
  en AOUT; si lo hace, agregar un divisor o alimentarlo a 3.3 V.

### 5.4 Alimentación conmutada de humedad (XY-MOS)

| Terminal XY-MOS | Conexión |
|---|---|
| VIN+ | 3V3 |
| VIN− | GND |
| OUT+ | VCC de los dos sensores de humedad |
| OUT− | GND de los dos sensores (o GND directo) |
| TRIG / EN | GPIO12 |
| GND de control | GND |

- **GPIO12 es strapping (MTDI):** debe estar bajo al arrancar. **Pull-down
  externo de 10 kΩ entre GPIO12 y GND es obligatorio**, no opcional.
- El firmware pone GPIO12 en **alto** solo durante la lectura (25 ms de
  estabilización + 12 muestras) y lo vuelve a bajo. Eso reduce la corrosión de
  las sondas.
- **Verificar el módulo antes de conectar los sensores:** con TRIG en bajo,
  OUT+ debe dar 0 V; con TRIG en alto (3.3 V), debe dar ≈3.3 V. Si el módulo
  conmuta masa en lugar de positivo, conectar el **GND** de los sensores a
  OUT− y su VCC directo a 3V3. El firmware asume conmutación de positivo.
- El módulo debe disparar con 3.3 V. Muchos XY-MOS necesitan 5 V en TRIG; si es
  el caso, usar un MOSFET de nivel lógico o un pequeño driver.

### 5.5 I2C (OLED y RTC)

| Señal | GPIO | OLED | DS3231 |
|---|---|---|---|
| SDA | 21 | SDA | SDA |
| SCL | 22 | SCL | SCL |
| VCC | 3V3 | VCC | VCC |
| GND | GND | GND | GND |
| Dirección | — | `0x3C` | `0x57` |

- Los módulos suelen traer pull-ups de 4.7 kΩ. **No agregar otros** si ya los
  tienen: en paralelo quedarían en 2.35 kΩ.
- Bus corto (menos de 30 cm) y lejos de los cables de potencia.
- El DS3231 lleva CR2032; sin batería la hora se pierde en cada corte.

### 5.6 Botones

| Botón | GPIO | Configuración | Otro terminal |
|---|---|---|---|
| Selector | 25 | `INPUT_PULLUP` interno | GND |
| Confirmar | 36 | `INPUT` + pull-up externo 10 kΩ a 3V3 | GND |
| Configuración | 39 | `INPUT` + pull-up externo 10 kΩ a 3V3 | GND |

GPIO36 y GPIO39 son solo entrada y sin pull-up interno: el pull-up externo no
es opcional. En reposo deben leer **3.3 V** con el botón sin pulsar; medirlo
antes de energizar.

El firmware aplica debounce en el bucle principal y, en el gesto de reset de
fábrica, exige 100 ms de lectura estable. Además diagnostica el fallo en el
arranque:

- si GPIO39 sigue en bajo 5 s después de soltar el botón, la OLED muestra
  `GPIO39 EN BAJO` y el arranque continúa normal. **Antes esta espera era un
  bucle sin tope**: con el pin en bajo permanente el equipo no arrancaba nunca.
- si GPIO36 nunca lee alto durante la ventana de Confirmar, muestra
  `GPIO36 EN BAJO` y cancela. Evita que un pin en corto borre la configuración
  sin que nadie pulsara nada.

---

## 6. Resistencias

| Cant. | Valor | Función |
|---|---|---|
| 8 | 10 kΩ | Pull-up de IN1-IN8 a 3V3 (relé apagado durante el arranque) |
| 1 | 10 kΩ | Pull-up de DATA del DHT21 |
| 1 | 10 kΩ | Pull-down de GPIO12 a GND (strapping) |
| 1 | 10 kΩ | Pull-up del sensor de ventana |
| 2 | 10 kΩ | Pull-up de los botones Confirmar y Configuración |
| 3 | 4.7 kΩ | Pull-up de DQ en cada DS18B20 |
| 2 | 4.7 kΩ | Pull-up de SDA y SCL (solo si los módulos no los traen) |

**Total: 13 × 10 kΩ y 5 × 4.7 kΩ.**

---

## 7. Lista rápida por GPIO

Para tener al lado mientras se cablea.

| Pin | Va a | Nota |
|---|---|---|
| GPIO4 | DATA del DHT21 | + 10 kΩ a 3V3 |
| GPIO5 | IN8 del módulo (alarma) | Strapping (no impide arrancar); pull-up 10 kΩ |
| GPIO12 | TRIG del XY-MOS | Strapping: 10 kΩ a GND obligatorio |
| GPIO13 | Sensor de ventana | 10 kΩ a 3V3; cerrada = LOW |
| GPIO14 | IN4 del módulo (cama caliente) | |
| GPIO16 | DQ del DS18B20 Z1 | 4.7 kΩ a 3V3 |
| GPIO17 | IN6 del módulo (ventilación) | |
| GPIO18 | IN3 del módulo (aspersión) | |
| GPIO19 | IN5 del módulo (iluminación) | |
| GPIO21 | SDA del OLED y DS3231 | |
| GPIO22 | SCL del OLED y DS3231 | |
| GPIO23 | IN7 del módulo (ventana) | Pulso de 5 s |
| GPIO25 | Botón Selector | `INPUT_PULLUP` interno |
| GPIO26 | IN1 del módulo (riego Z1) | |
| GPIO27 | IN2 del módulo (riego Z2) | |
| GPIO32 | DQ del DS18B20 Z2 | 4.7 kΩ a 3V3 |
| GPIO33 | DQ del DS18B20 cama | 4.7 kΩ a 3V3 |
| GPIO34 | AOUT humedad Z1 | ADC1, solo entrada |
| GPIO35 | AOUT humedad Z2 | ADC1, solo entrada |
| GPIO36 | Botón Confirmar | Solo entrada; 10 kΩ a 3V3 |
| GPIO39 | Botón Configuración | Solo entrada; 10 kΩ a 3V3 |
| 5V / VIN | +5 V de la fuente | No alimentar por USB a la vez |
| 3V3 | Riel de 3.3 V | Solo sensores y pull-ups |
| GND | Masa común | Fuente, relés, sensores y botones |

Reservados: GPIO0 (boot), GPIO1/3 (UART), GPIO6-11 (flash). Libres: GPIO2 y
GPIO15 (ambos strapping), no se usan en v1.

---

## 8. Verificaciones antes de energizar cargas

1. [ ] Continuidad de GND entre fuente, ESP32, módulo de relés y sensores.
2. [ ] **Sin** continuidad entre +5 V y GND.
3. [ ] Fuente ajustada a 5.0-5.1 V; medir antes de conectar el ESP32.
4. [ ] GPIO12 en bajo al arrancar (pull-down de 10 kΩ presente).
5. [ ] Verificar que los 8 relés quedan OFF al arrancar (pull-ups en IN1-IN8).
6. [ ] Tensión en los IN del módulo ≤ 3.4 V (ver §5.1).
7. [ ] **3.3 V en reposo en GPIO36 y GPIO39** con los botones sin pulsar
       (pull-ups externos). Sin ellos el gesto de reset no es confiable.
8. [ ] Los 8 relés quedan **apagados** al energizar, antes de conectar cargas.
9. [ ] Un relé por vez: probar desde el portal y escuchar el clic.
10. [ ] Relé 7: se energiza 5 s y se suelta solo.
11. [ ] Medir OUT+ del XY-MOS en bajo y en alto (§5.4).
12. [ ] Escanear I2C y confirmar `0x3C` y `0x57`.
13. [ ] Probar cada sensor desde el portal (`/api/sensors`) y calibrar humedad.
14. [ ] Probar el gesto de reset: mantener Configuración 10 s, soltar y
        pulsar Confirmar. Y probar que **sin** el pull-up de GPIO39 el equipo
        igual arranca (debe mostrar `GPIO39 EN BAJO` y seguir).
15. [ ] Recién después conectar cargas, empezando por una lámpara.

---

## 9. Advertencias

- **220 V no se prueba en protoboard.** El módulo de relés no es un tablero
  eléctrico: contactores, fusibles, bornera, tierra y gabinete son
  obligatorios.
- **GPIO12 es el strapping crítico:** si queda alto al arrancar, `VDD_SDIO`
  pasa a 1.8 V y el ESP32 **no arranca**. Es el fallo más común de este
  montaje. GPIO5 también es strapping pero **no condiciona el arranque**: su
  pull-up importa por la polaridad *low-level*, no por el boot.
- **Ventana:** un pulso en el estado equivocado mueve la ventana al revés. No
  habilitar la automatización hasta confirmar el datasheet del controlador y
  probar el sensor de cerrada.
- **Sensores de humedad resistivos:** se corroen aunque se alimenten por
  pulsos. Preferir capacitivos.
- **Cama caliente:** el corte absoluto a 35 °C es de firmware, no reemplaza una
  protección térmica independiente (termostato de seguridad o fusible térmico).
- **Alarma:** si se corta la alimentación del ESP32, la alarma deja de sonar.
  Si el riesgo lo exige, usar una sirena autoalimentada.
