#pragma once

#include <cstddef>

#include "esp_err.h"

namespace gate::network {

struct SetupAccessPoint {
  const char* password;
  bool station_credentials_present;
};

esp_err_t start(const SetupAccessPoint& config);
bool station_connected();
const char* access_point_ssid();

}  // namespace gate::network
