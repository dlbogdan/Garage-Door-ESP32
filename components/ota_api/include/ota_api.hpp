#pragma once

#include "app_config.hpp"
#include "esp_err.h"
#include "esp_http_server.h"

namespace gate::ota_api {

struct Context {
  const gate::config::AppConfig* config;
  void (*schedule_restart)();
};

esp_err_t register_routes(httpd_handle_t server, Context context);

}  // namespace gate::ota_api
