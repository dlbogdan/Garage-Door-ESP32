#include "management_server.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "management_api.hpp"
#include "ota_api.hpp"
#include "setup_api.hpp"

namespace gate::management {
namespace {

gate::bootstrap::Credentials credentials;
gate::config::AppConfig active_config;
bool application_provisioned = false;
httpd_handle_t server = nullptr;

extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[] asm("_binary_index_html_end");
extern const unsigned char app_js_start[] asm("_binary_app_js_start");
extern const unsigned char app_js_end[] asm("_binary_app_js_end");
extern const unsigned char app_css_start[] asm("_binary_app_css_start");
extern const unsigned char app_css_end[] asm("_binary_app_css_end");

bool decode_form_value(const std::string& encoded, std::string* decoded) {
  decoded->clear();
  decoded->reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const char value = encoded[index];
    if (value == '+') {
      decoded->push_back(' ');
    } else if (value == '%' && index + 2 < encoded.size() &&
               std::isxdigit(static_cast<unsigned char>(encoded[index + 1])) &&
               std::isxdigit(static_cast<unsigned char>(encoded[index + 2]))) {
      unsigned int byte = 0;
      std::sscanf(encoded.substr(index + 1, 2).c_str(), "%02x", &byte);
      if (byte == 0) return false;
      decoded->push_back(static_cast<char>(byte));
      index += 2;
    } else if (value == '%') {
      return false;
    } else {
      decoded->push_back(value);
    }
  }
  return true;
}

bool form_value(const std::string& body, const char* key, std::string* value) {
  const std::string prefix = std::string(key) + "=";
  std::size_t start = 0;
  while (start <= body.size()) {
    const std::size_t end = body.find('&', start);
    const std::string field = body.substr(start, end - start);
    if (field.rfind(prefix, 0) == 0) {
      return decode_form_value(field.substr(prefix.size()), value);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

bool parse_integer(const std::string& body, const char* key, int minimum,
                   int maximum, int* value) {
  std::string text;
  if (!form_value(body, key, &text) || text.empty()) return false;
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed < minimum || parsed > maximum) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

std::string json_escape(const char* input) {
  std::string output;
  for (; input != nullptr && *input != '\0'; ++input) {
    if (*input == '"' || *input == '\\') output.push_back('\\');
    if (static_cast<unsigned char>(*input) >= 0x20) output.push_back(*input);
  }
  return output;
}

esp_err_t send_error(httpd_req_t* request, const char* status,
                     const std::string& message) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, message.c_str(), message.size());
}

esp_err_t send_asset(httpd_req_t* request, const char* content_type,
                     const unsigned char* start, const unsigned char* end) {
  httpd_resp_set_type(request, content_type);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
  return httpd_resp_send(request, reinterpret_cast<const char*>(start), end - start);
}

esp_err_t root_handler(httpd_req_t* request) {
  return send_asset(request, "text/html; charset=utf-8", index_html_start,
                    index_html_end);
}
esp_err_t javascript_handler(httpd_req_t* request) {
  return send_asset(request, "text/javascript; charset=utf-8", app_js_start,
                    app_js_end);
}
esp_err_t stylesheet_handler(httpd_req_t* request) {
  return send_asset(request, "text/css; charset=utf-8", app_css_start, app_css_end);
}
esp_err_t captive_handler(httpd_req_t* request) {
  httpd_resp_set_status(request, "302 Found");
  httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
  return httpd_resp_send(request, nullptr, 0);
}

void restart_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(1200));
  esp_restart();
}
void schedule_restart() {
  xTaskCreate(restart_task, "management_restart", 2048, nullptr, 5, nullptr);
}

esp_err_t register_assets() {
  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = nullptr},
      {.uri = "/app.js", .method = HTTP_GET, .handler = javascript_handler, .user_ctx = nullptr},
      {.uri = "/app.css", .method = HTTP_GET, .handler = stylesheet_handler, .user_ctx = nullptr},
  };
  for (const auto& route : routes) {
    const esp_err_t result = httpd_register_uri_handler(server, &route);
    if (result != ESP_OK) return result;
  }
  return ESP_OK;
}

esp_err_t start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // The authenticated handlers use bounded C++ configuration/diagnostic
  // snapshots and JSON/form parsing. ESP-IDF's 4096-byte default can corrupt
  // the HTTP task's FreeRTOS list state under those legitimate call chains
  // before the stack canary gets a chance to report an overflow.
  config.stack_size = 12288;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 22;
  esp_err_t result = httpd_start(&server, &config);
  if (result != ESP_OK) return result;

  if ((result = register_assets()) == ESP_OK) {
    result = gate::setup_api::register_routes(
        server, {&credentials, &application_provisioned, send_error, form_value,
                 parse_integer, json_escape, schedule_restart});
  }
  if (result == ESP_OK) {
    result = gate::management_api::register_routes(
        server, {&credentials, &active_config, &application_provisioned, send_error,
                 form_value, parse_integer, json_escape, schedule_restart});
  }
  // OTA must precede the wildcard captive route so firmware endpoints remain reachable.
  if (result == ESP_OK) {
    result = gate::ota_api::register_routes(server, {&active_config, schedule_restart});
  }
  const httpd_uri_t captive{.uri = "/*", .method = HTTP_GET,
                            .handler = captive_handler, .user_ctx = nullptr};
  if (result == ESP_OK) result = httpd_register_uri_handler(server, &captive);
  if (result != ESP_OK) {
    httpd_stop(server);
    server = nullptr;
  }
  return result;
}

}  // namespace

esp_err_t start(const gate::bootstrap::Credentials& initial_credentials,
                bool provisioned, const gate::config::AppConfig* config) {
  credentials = initial_credentials;
  application_provisioned = provisioned;
  if (config != nullptr) active_config = *config;
  return start_http_server();
}

}  // namespace gate::management
