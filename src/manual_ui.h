#ifndef MANUAL_UI_H
#define MANUAL_UI_H

#include <stdint.h>

#include "config.h"

// ============================================================================
// manual_ui.h — Menú local con Selector/Confirmar
// Fuente: docs/03_reglas_y_plan.md §3.
// - Selector recorre solo los canales habilitados (la alarma no está en la
//   lista): riego_z1, riego_z2, aspersion, cama_caliente, iluminacion,
//   ventilacion, ventana.
// - Confirmar ejecuta la acción del canal mostrado.
// - Timeout de MANUAL_TIMEOUT_MS sin botones → reposo.
// ============================================================================

void manualUiInit();
void manualUiUpdate();

bool manualUiIsSelecting();
void manualUiForceIdle();

// Canal actualmente mostrado (válido solo si manualUiIsSelecting()).
RelayChannel manualUiGetSelectedChannel();

#endif // MANUAL_UI_H
