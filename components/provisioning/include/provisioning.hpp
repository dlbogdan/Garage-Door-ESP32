#pragma once

#include "esp_err.h"

namespace gate::provisioning {

// Starts a WPA2 setup AP, captive HTTP portal, and optional station connection.
// The generated AP password and submitted station credentials are persisted in
// a bootstrap NVS namespace until the complete application wizard is available.
esp_err_t start();

}  // namespace gate::provisioning

