#pragma once

#include <string>

#include "app_config.hpp"
#include "esp_http_server.h"

namespace gate::web_auth {

bool authorize(httpd_req_t* request, bool require_csrf = false,
               bool refresh_activity = true);
bool verify_reauthorization(httpd_req_t* request,
                            const gate::config::AdminConfig& admin);
void create_session();
void clear_session();
const std::string& token();
const std::string& csrf();

}  // namespace gate::web_auth
