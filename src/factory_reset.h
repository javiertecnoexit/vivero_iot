#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

// ============================================================================
// factory_reset.h — Borrado de configuración persistida (M5)
// Fuente: docs/03_reglas_y_plan.md §11.3.
// - Reset total: WiFi, AP, provisioning, backend, perfiles, calibración,
//   canales, cama y latches. El llamador debe reiniciar después.
// - Borrado de solo WiFi: conserva perfiles, canales, calibración e identidad.
// - Ninguna función reinicia: el portal programa el reinicio y el gesto de
//   arranque lo hace al terminar el borrado.
// ============================================================================

// Borra toda la configuración persistida y restaura defaults en RAM.
void factoryResetAll();

// Borra solo las credenciales WiFi (STA) y la clave del AP.
void factoryResetWifiOnly();

#endif // FACTORY_RESET_H
