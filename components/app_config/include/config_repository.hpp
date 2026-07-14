#pragma once

#include "app_config.hpp"
#include "esp_err.h"

namespace gate::config {

class ConfigRepository {
 public:
  esp_err_t load(AppConfig* config) const;
  esp_err_t save(const AppConfig& config) const;
  esp_err_t erase() const;
};

}  // namespace gate::config
