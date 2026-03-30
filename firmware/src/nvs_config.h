#pragma once

#include "types.h"

/**
 * @brief Load all configuration fields from NVS into the provided struct.
 *
 * Missing keys are initialised with the compile-time defaults defined in
 * config.h.  Call once during setup() before starting tasks.
 *
 * @param cfg  Output struct to fill.
 */
void nvs_load(SensorConfig &cfg);

/**
 * @brief Persist all configuration fields in the provided struct to NVS.
 *
 * Safe to call from any core; Preferences is internally mutex-protected.
 *
 * @param cfg  Configuration to save.
 */
void nvs_save(const SensorConfig &cfg);
