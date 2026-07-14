#pragma once

#include "app_config.hpp"
#include "esp_err.h"

namespace gate::hardware {

// Initializes the configured relay at its inactive level and starts read-only
// monitoring of the closed-position sensor. This milestone never pulses relay.
esp_err_t start_monitoring(const gate::config::AppConfig& config);
bool monitoring_active();
bool closed_sensor_active();

}  // namespace gate::hardware
