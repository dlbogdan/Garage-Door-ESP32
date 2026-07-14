#pragma once

#include "app_config.hpp"
#include "esp_err.h"

namespace gate::homekit {

// Initializes Arduino and HomeSpan before provisioning. Arduino/HomeSpan owns
// creation of the Wi-Fi driver, default netifs, and Arduino network events.
void initialize_networking();
esp_err_t start(const gate::config::AppConfig& config);
bool active();
bool paired();

}  // namespace gate::homekit
