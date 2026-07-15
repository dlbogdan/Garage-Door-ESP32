#pragma once

#include <string>

#include "app_config.hpp"
#include "bootstrap_credentials.hpp"
#include "esp_http_server.h"

namespace gate::setup_api {

struct Context {
  gate::bootstrap::Credentials* credentials;
  bool* application_provisioned;
  esp_err_t (*send_error)(httpd_req_t*, const char*, const std::string&);
  bool (*form_value)(const std::string&, const char*, std::string*);
  bool (*parse_integer)(const std::string&, const char*, int, int, int*);
  std::string (*json_escape)(const char*);
  void (*schedule_restart)();
};

esp_err_t register_routes(httpd_handle_t server, Context context);

}  // namespace gate::setup_api
