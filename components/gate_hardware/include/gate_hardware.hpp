#pragma once

#include "app_config.hpp"
#include "esp_err.h"

namespace gate::hardware {

using SensorChangedCallback = void (*)(bool active, void* context);

// Initializes the relay inactive and starts debounced sensor monitoring.
esp_err_t start_monitoring(const gate::config::AppConfig& config,
                           SensorChangedCallback callback = nullptr,
                           void* callback_context = nullptr);
bool monitoring_active();
bool feedback_active();

// Low-level fail-safe primitives. The serialized controller runtime is their
// sole caller; no command-facing component is permitted to drive the relay.
esp_err_t activate_relay();
esp_err_t deactivate_relay();
bool relay_active();

}  // namespace gate::hardware
