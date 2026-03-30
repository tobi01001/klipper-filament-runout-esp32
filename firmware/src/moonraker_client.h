#pragma once

#include "types.h"

/**
 * @brief Poll the Moonraker API for the current extruder velocity.
 *
 * Sends an HTTP GET to:
 *   http://<ip>:<port>/printer/objects/query?extruder
 *
 * Parses `result.status.extruder.velocity` from the JSON response.
 * Returns 0.0f on any network or parse error so the fault detector treats
 * the extruder as idle (safe fallback – no spurious runouts).
 *
 * @param cfg  Live sensor configuration (moonraker_ip, moonraker_port).
 * @return     Extruder velocity in mm/s, or 0.0f on error.
 */
float moonraker_poll(const SensorConfig &cfg);
