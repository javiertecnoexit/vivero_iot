#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

// ============================================================================
// config.h — Pines, constantes y valores de fábrica
// Proyecto: vivero_iot (ESP32 DevKit V1)
// Fuente de verdad: docs/01_hardware_pinout.md, docs/02_protocolo_ws.md y
// docs/03_reglas_y_plan.md.
// ============================================================================

// ----------------------------------------------------------------------------
// Identidad del firmware
// ----------------------------------------------------------------------------
#define FIRMWARE_PROJECT       "vivero_iot"
#define FIRMWARE_VERSION       "0.1.0-m6"

// ----------------------------------------------------------------------------
// Pines — botones
// ----------------------------------------------------------------------------
// GPIO34-39 son entradas-only SIN pull-up interno → usar INPUT (pull externo).
// GPIO25 sí tiene pull-up interno → INPUT_PULLUP.
#define PIN_BTN_SELECTOR       25   // Selector (INPUT_PULLUP)
#define PIN_BTN_CONFIRM        36   // Confirmar (INPUT, sin pull interno)
#define PIN_BTN_CONFIG         39   // Configuración (INPUT, sin pull interno)

// ----------------------------------------------------------------------------
// Pines — I2C (pantalla OLED, RTC)
// ----------------------------------------------------------------------------
#define PIN_I2C_SDA            21
#define PIN_I2C_SCL            22

// ----------------------------------------------------------------------------
// Pines — 8 relés fijos (docs/01_hardware_pinout.md §1)
// ----------------------------------------------------------------------------
// Módulo opto-acoplado de 8 canales, low-level trigger.
// El canal 7 (ventana) NO alimenta el motor: entrega un pulso de 5 s a un
// controlador propio que alterna el sentido (abrir/cerrar). Ver window.h.
#define PIN_RELAY_RIEGO_Z1     26   // Relé 1 — riego sustrato Z1
#define PIN_RELAY_RIEGO_Z2     27   // Relé 2 — riego sustrato Z2
#define PIN_RELAY_ASPERSION    18   // Relé 3 — riego por aspersión
#define PIN_RELAY_CAMA         14   // Relé 4 — cama caliente
#define PIN_RELAY_ILUMINACION  19   // Relé 5 — iluminación
#define PIN_RELAY_VENTILACION  17   // Relé 6 — ventilación forzada
#define PIN_RELAY_VENTANA      23   // Relé 7 — pulso al controlador de ventana
#define PIN_RELAY_ALARMA       5    // Relé 8 — alarma (GPIO5 es strapping)

// ----------------------------------------------------------------------------
// Canales de relé (fijos en firmware, no reconfigurables por el usuario)
// ----------------------------------------------------------------------------
// El portal solo permite habilitar/deshabilitar y probar cada canal; la
// función y el GPIO quedan fijados acá.
enum class RelayChannel : uint8_t {
    RIEGO_Z1     = 0,
    RIEGO_Z2     = 1,
    ASPERSION    = 2,
    CAMA_CALIENTE = 3,
    ILUMINACION  = 4,
    VENTILACION  = 5,
    VENTANA      = 6,
    ALARMA       = 7,
    COUNT        = 8
};

#define RELAY_CHANNEL_COUNT    8

// ----------------------------------------------------------------------------
// Pines — sensores (docs/01_hardware_pinout.md §4)
// ----------------------------------------------------------------------------
#define PIN_SOIL_ADC_1         34   // Humedad sustrato Z1 (ADC1, solo entrada)
#define PIN_SOIL_ADC_2         35   // Humedad sustrato Z2 (ADC1, solo entrada)
#define PIN_SENSOR_POWER       12   // Alimentación conmutada humedad (XY-MOS)
                                    // GPIO12 strapping MTDI: pull-down 10 kΩ
#define PIN_WINDOW_CLOSED      13   // Sensor de ventana cerrada (LOW = cerrada)
#define PIN_DS18B20_Z1         16   // DS18B20 temperatura sustrato Z1
#define PIN_DS18B20_Z2         32   // DS18B20 temperatura sustrato Z2
#define PIN_DS18B20_CAMA       33   // DS18B20 temperatura cama caliente
#define PIN_DHT21              4    // DHT21/AM2301 de aire

// Modelo de sensor de aire:
//   1 = DHT11  (solo banco/pruebas)
//   2 = DHT21  (sensor final del proyecto)
#define AIR_SENSOR_MODEL       2

#define MAX_SUBSTRATE_ZONES    2    // Z1 y Z2 (humedad + temperatura)
#define MAX_SPRINKLER_ZONES    1    // Una sola aspersión
#define MAX_DS18B20            3    // Z1, Z2 y cama caliente (un pin por sonda)

// ----------------------------------------------------------------------------
// Lectura de sensores — humedad de sustrato (docs/03_reglas_y_plan.md)
// ----------------------------------------------------------------------------
// Estabilización tras encender GPIO12: 20-30 ms (delay() bloqueante
// intencional y comentado, única excepción a la regla 1).
#define SOIL_STABILIZE_MS      25UL
// Lecturas ADC a promediar (8-16; usamos 12).
#define SOIL_ADC_SAMPLES       12
// Rango común de fábrica para sensores resistivos (más seco = ADC más alto).
// Calibrado=false hasta que el portal de calibración lo ajuste por zona.
#define SOIL_DEFAULT_DRY_RAW   3000
#define SOIL_DEFAULT_WET_RAW   1200

// ----------------------------------------------------------------------------
// Modos de operación (docs/03_reglas_y_plan.md §2)
// ----------------------------------------------------------------------------
// - NORMAL: automatización activa; puede haber STA o modo offline.
// - PORTAL: AP cautivo para configuración. La automatización NO se suspende.
// - MANTENIMIENTO: automatización suspendida; relés de proceso en OFF.
enum class OperationMode : uint8_t {
    NORMAL        = 0,
    PORTAL        = 1,
    MANTENIMIENTO = 2,
    // Compatibilidad temporal con la base (se retira al completar M1).
    CONFIG        = PORTAL
};

// ----------------------------------------------------------------------------
// Ciclo local
// ----------------------------------------------------------------------------
#define LOCAL_CYCLE_MS         30000UL   // 30 s

// ----------------------------------------------------------------------------
// Hora local para pantalla y evaluación de horarios
// ----------------------------------------------------------------------------
// El RTC y los payloads ISO 8601 viajan en UTC. Este offset se aplica para
// mostrar la hora y para evaluar horarios locales. Argentina no aplica DST.
#define LOCAL_TZ_OFFSET_HOURS  (-3)
// Alias histórico usado por el display.
#define DISPLAY_TZ_OFFSET_HOURS LOCAL_TZ_OFFSET_HOURS

// ----------------------------------------------------------------------------
// Portal local (AP cautivo)
// ----------------------------------------------------------------------------
// SSID: prefijo + últimos 4 caracteres de la MAC.
#define AP_SSID_PREFIX         "Vivero-IoT-"
// Clave inicial: se deriva de la MAC y se muestra en la OLED (M1). El portal
// permite cambiarla. Con WPA2 y clave válida (>= 8 caracteres).
#define AP_PASSWORD_MIN_LEN    8
#define PORTAL_TIMEOUT_MS      900000UL  // 15 min sin actividad → cerrar AP
#define PORTAL_WARN_MS         780000UL  // ~13 min → aviso en OLED
#define PORTAL_TEST_MS         10000UL   // prueba de canal desde el portal (auto-off)
#define PORTAL_TEST_MAINT_MS   30000UL   // prueba en MANTENIMIENTO (auto-off, doc §2.2)
// La base usaba CONFIG_TIMEOUT_MS; se mantiene el alias hasta completar M1.
#define CONFIG_TIMEOUT_MS      PORTAL_TIMEOUT_MS
#define CONFIG_WARN_MS         PORTAL_WARN_MS
// Auto-recuperación: si tras guardar WiFi no conecta en 60 s, reabre el portal.
#define WIFI_CONNECT_TIMEOUT_MS 60000UL

// ----------------------------------------------------------------------------
// Valores de fábrica — riego
// ----------------------------------------------------------------------------
#define DEFAULT_SUBSTRATE_ZONES 2
#define DEFAULT_SPRINKLER_ZONES 1
#define DEFAULT_IRRIGATION_S   60        // 1 minuto
#define DEFAULT_ALARM_MAX_TEMP  45.0f
#define DEFAULT_ALARM_MIN_TEMP  0.0f
#define DEFAULT_MIN_HUMIDITY    20

// ----------------------------------------------------------------------------
// Ventana — pulso toggle (docs/01_hardware_pinout.md §5)
// ----------------------------------------------------------------------------
// El controlador alterna el sentido en cada pulso. Nunca dejar el relé
// energizado de forma continua.
#define WINDOW_PULSE_MS        5000UL    // duración del pulso
#define WINDOW_MIN_INTERVAL_MS 60000UL   // mínimo entre pulsos (ajustar datasheet)
#define WINDOW_MOVE_TIMEOUT_MS 20000UL   // espera de transición del sensor
#define WINDOW_SENSOR_DEBOUNCE_MS 50UL   // debounce del sensor de cerrada
#define WINDOW_CLOSED_LEVEL    LOW       // switch/reed a GND → cerrada = LOW

// ----------------------------------------------------------------------------
// Cama caliente — termostato on/off con histéresis
// ----------------------------------------------------------------------------
#define DEFAULT_BED_TARGET_C       25.0f
#define BED_THERMOSTAT_HYST_C      1.0f
#define BED_MAX_C                  35.0f     // corte absoluto, no anulable
#define BED_MIN_ON_MS              60000UL   // anti-ciclado
#define BED_MIN_OFF_MS             120000UL
#define BED_SENSOR_FAULT_TIMEOUT_MS 300000UL  // 5 min sin lectura válida → OFF

// ----------------------------------------------------------------------------
// Ventilación — clima (OR de temperatura y humedad)
// ----------------------------------------------------------------------------
#define DEFAULT_VENT_ON_TEMP_C     30.0f
#define DEFAULT_VENT_OFF_TEMP_C    28.0f
#define DEFAULT_VENT_ON_RH_PCT     80.0f
#define DEFAULT_VENT_OFF_RH_PCT    70.0f
#define VENT_SUSTAIN_CYCLES        2        // 2 ciclos locales = 60 s

// ----------------------------------------------------------------------------
// Riego — interlocks y límites duros (docs/03_reglas_y_plan.md §4.2 y §10.1)
// ----------------------------------------------------------------------------
// Un solo riego a la vez (Z1, Z2 y aspersión) y espera entre zonas.
#define IRRIGATION_INTERLOCK_DEFAULT   true
#define IRRIGATION_STAGGER_MS          30000UL
// Duración máxima por zona (se recorta el pedido; al corte se alarma).
#define IRRIGATION_MAX_RUN_S_DEFAULT   900
#define SPRINKLER_MAX_RUN_S_DEFAULT    1800
#define IRRIGATION_MAX_RUN_ABS_S       3600
// Tiempo mínimo apagado entre ciclos automáticos por umbral.
// El máximo debe caber en uint16_t (perfil local y runtime).
#define IRRIGATION_MIN_OFF_S_DEFAULT   1800
#define IRRIGATION_MIN_OFF_MAX_S       43200   // 12 h

// ----------------------------------------------------------------------------
// Iluminación — corte duro de continuidad
// ----------------------------------------------------------------------------
#define LIGHT_MAX_ON_MS        (16UL * 3600UL * 1000UL)  // 16 h continuas

// ----------------------------------------------------------------------------
// Ventilación — tiempos mínimos y máximo continuo
// ----------------------------------------------------------------------------
#define VENT_MIN_ON_MS         60000UL
#define VENT_MIN_OFF_MS        120000UL
#define VENT_MAX_RUN_MS        3600000UL   // 1 h

// ----------------------------------------------------------------------------
// Alarmas
// ----------------------------------------------------------------------------
// Humedad persistentemente baja: ciclos locales consecutivos bajo el mínimo
// antes de disparar (falla de válvula/sensor pese a los intentos de riego).
#define SOIL_HUMIDITY_PERSISTENT_CYCLES  120   // 60 min a 30 s
// Segundos sin contacto exitoso con el backend antes de alarmar.
#define ALARM_NO_CONNECTION_SEC   3600UL
// Ciclos locales con lectura inválida antes de disparar sensor_fault
// (3 ciclos = 90 s, evita falsos positivos por una lectura aislada).
#define ALARM_SENSOR_FAULT_CYCLES 3
// Silencio del relé de alarma sin borrar condiciones (10 min).
#define ALARM_SILENCE_MS          600000UL
// Mantener Confirmar este tiempo silencia la alarma (sin web).
#define ALARM_SILENCE_HOLD_MS      3000UL

// ----------------------------------------------------------------------------
// Riego manual y eventos
// ----------------------------------------------------------------------------
#define MANUAL_TIMEOUT_MS       5000UL   // 5 s sin botones → cancelar selección
#define MAX_IRRIGATION_EVENTS   32       // FIFO de eventos para idempotencia
#define MAX_SCHEDULES_PER_ZONE  4        // horarios diarios por zona
// Días de la semana de un horario: bit 0 = lunes ... bit 6 = domingo.
#define SCHEDULE_DAYS_ALL       0x7F
#define SCHEDULE_DAYS_NONE      0x00

// ----------------------------------------------------------------------------
// Capa de red / backend
// ----------------------------------------------------------------------------
#define CLOUD_CYCLE_DEFAULT_MS  300000UL // 5 minutos
#define CLOUD_BACKOFF_BASE_S    30
#define CLOUD_BACKOFF_MAX_MS    1800000UL
#define CLOUD_HTTP_TIMEOUT_MS   10000UL
#define MAX_TELEMETRY_READINGS  64
#define MAX_OVERRIDES_BATCH     8

// ----------------------------------------------------------------------------
// M6 — Cliente WebSocket (docs/02_protocolo_ws.md)
// ----------------------------------------------------------------------------
#define WS_HEARTBEAT_DEFAULT_S      25
#define WS_TELEMETRY_DEFAULT_S      30
#define WS_TELEMETRY_MIN_S          10
#define WS_BACKOFF_BASE_MS          2000UL
#define WS_BACKOFF_MAX_MS           60000UL
// Con TLS el handshake suma latencia (Cloudflare, ~2-5 s desde el ESP32):
// 10 s quedaba al filo y disparaba un reintento injustificado.
#define WS_CONNECT_TIMEOUT_MS       20000UL
#define WS_FRAME_MAX_BYTES          16384
#define WS_RX_IDLE_TIMEOUT_MS       20000UL   // sin RX antes de welcome
#define WS_TELEMETRY_QUEUE          720       // ~6 h a 30 s (RAM)
#define WS_TELEMETRY_BATCH          20
#define WS_EVENT_QUEUE              64
#define WS_CRITICAL_EVENT_QUEUE     8         // ring persistido en NVS
#define WS_COMMAND_CACHE            8         // ids de comando recordados
#define WS_COMMAND_CACHE_MS         600000UL  // 10 min
#define WS_RATE_MAX_PER_MIN         30
#define WS_RATE_CHANNEL_MS          2000UL
// Claim por HTTPS: el handshake TLS va antes del POST (handoff del backend §3.4).
#define WS_CLAIM_RETRY_MS           60000UL
#define WS_CLAIM_TIMEOUT_MS         15000UL
#define WS_STATE_REFRESH_MS         300000UL  // 5 min
#define WS_CLAIM_PATH               "/api/v1/devices/claim"

// ----------------------------------------------------------------------------
// Relé — polaridad (confirmado en hardware real)
// ----------------------------------------------------------------------------
// Low-level trigger: IN en 0 V = relé activado; 3.3 V = apagado.
#define RELAY_ACTIVE_LOW        true

#endif // CONFIG_H
