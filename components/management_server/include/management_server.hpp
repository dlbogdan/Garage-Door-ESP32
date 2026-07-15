#pragma once

#include "app_config.hpp"
#include "bootstrap_credentials.hpp"
#include "esp_err.h"

namespace gate::management {

esp_err_t start(const gate::bootstrap::Credentials& credentials,
                bool application_provisioned,
                const gate::config::AppConfig* active_config);

}  // namespace gate::management
