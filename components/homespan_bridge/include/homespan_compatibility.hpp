#pragma once

#include "app_config.hpp"
#include "esp_err.h"

namespace gate::homekit {

// Initializes Arduino and HomeSpan before provisioning. Arduino/HomeSpan owns
// creation of the Wi-Fi driver, default netifs, and Arduino network events.
void initialize_networking();
// Starts the Arduino-owned station connection used during staged onboarding.
// This does not initialize HomeKit or create an accessory.
esp_err_t connect_bootstrap_station(const char* ssid, const char* password);
esp_err_t start(const gate::config::AppConfig& config);
bool active();
bool paired();

}  // namespace gate::homekit
