#pragma once

#include "esp_err.h"

namespace gate::captive_dns {

esp_err_t start();
void stop();
bool active();

}  // namespace gate::captive_dns
