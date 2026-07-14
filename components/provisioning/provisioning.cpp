#include "provisioning.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "WiFi.h"
#include "app_config.hpp"
#include "config_repository.hpp"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gate_hardware.hpp"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "password.hpp"
#include "gate_runtime.hpp"
#include "homespan_compatibility.hpp"

namespace gate::provisioning {
namespace {

constexpr char kTag[] = "provisioning";
constexpr char kNamespace[] = "gate_boot";
constexpr char kCredentialsKey[] = "credentials";
constexpr std::uint32_t kMagic = 0x42535450;
constexpr std::size_t kMaxRequestBody = 2048;

#pragma pack(push, 1)
struct BootstrapCredentials {
  std::uint32_t magic{kMagic};
  char ap_password[16]{};
  char station_ssid[33]{};
  char station_password[64]{};
};
#pragma pack(pop)

BootstrapCredentials credentials;
char access_point_ssid[24]{};
httpd_handle_t server = nullptr;
bool station_connected = false;
bool application_provisioned = false;
gate::config::AppConfig active_config;

void restart_task(void*);

struct Session {
  bool active{false};
  std::string token;
  std::string csrf;
  std::int64_t created_us{0};
  std::int64_t last_seen_us{0};
};

Session session;
constexpr std::int64_t kSessionIdleUs = 30LL * 60 * 1000000;
constexpr std::int64_t kSessionAbsoluteUs = 8LL * 60 * 60 * 1000000;

extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[] asm("_binary_index_html_end");
extern const unsigned char app_js_start[] asm("_binary_app_js_start");
extern const unsigned char app_js_end[] asm("_binary_app_js_end");
extern const unsigned char app_css_start[] asm("_binary_app_css_start");
extern const unsigned char app_css_end[] asm("_binary_app_css_end");

template <typename Character, std::size_t Size>
void copy_string(Character (&destination)[Size], const char* source) {
  std::memset(destination, 0, Size);
  std::memcpy(destination, source,
              std::min(std::strlen(source), Size - 1));
}

template <typename Character, std::size_t Size>
void copy_string(Character (&destination)[Size], const std::string& source) {
  copy_string(destination, source.c_str());
}

esp_err_t save_credentials() {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, kCredentialsKey, &credentials,
                          sizeof(credentials));
  }
  if (result == ESP_OK) result = nvs_commit(handle);
  if (handle != 0) nvs_close(handle);
  return result;
}

void generate_ap_password() {
  constexpr char alphabet[] =
      "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  for (std::size_t index = 0; index < sizeof(credentials.ap_password) - 1;
       ++index) {
    credentials.ap_password[index] =
        alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  credentials.ap_password[sizeof(credentials.ap_password) - 1] = '\0';
}

esp_err_t load_or_create_credentials() {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (result == ESP_OK) {
    std::size_t size = sizeof(credentials);
    result = nvs_get_blob(handle, kCredentialsKey, &credentials, &size);
    nvs_close(handle);
    if (result == ESP_OK && size == sizeof(credentials) &&
        credentials.magic == kMagic &&
        strnlen(credentials.ap_password, sizeof(credentials.ap_password)) >= 8) {
      return ESP_OK;
    }
  }

  credentials = {};
  credentials.magic = kMagic;
  generate_ap_password();
  return save_credentials();
}

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

gate::config::ActiveLevel parse_level(const std::string& value) {
  return value == "high" ? gate::config::ActiveLevel::kHigh
                         : gate::config::ActiveLevel::kLow;
}

gate::config::SensorPull parse_pull(const std::string& value) {
  if (value == "none") return gate::config::SensorPull::kNone;
  if (value == "down") return gate::config::SensorPull::kDown;
  return gate::config::SensorPull::kUp;
}

esp_err_t send_asset(httpd_req_t* request, const char* content_type,
                     const unsigned char* start, const unsigned char* end,
                     bool cache) {
  httpd_resp_set_type(request, content_type);
  httpd_resp_set_hdr(request, "Cache-Control", cache ? "public, max-age=3600"
                                                     : "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
  return httpd_resp_send(request, reinterpret_cast<const char*>(start), end - start);
}

esp_err_t send_error(httpd_req_t* request, const char* status,
                     const std::string& message) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, message.c_str(), message.size());
}

std::string json_escape(const char* input) {
  std::string output;
  for (; *input != '\0'; ++input) {
    if (*input == '"' || *input == '\\') output.push_back('\\');
    if (static_cast<unsigned char>(*input) >= 0x20) output.push_back(*input);
  }
  return output;
}

std::string random_hex(std::size_t bytes) {
  constexpr char hex[] = "0123456789abcdef";
  std::string output(bytes * 2, '0');
  for (std::size_t index = 0; index < bytes; ++index) {
    const std::uint8_t value = static_cast<std::uint8_t>(esp_random());
    output[index * 2] = hex[value >> 4];
    output[index * 2 + 1] = hex[value & 0x0f];
  }
  return output;
}

bool constant_time_equal(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
  }
  return difference == 0;
}

bool request_cookie(httpd_req_t* request, const char* name, std::string* value) {
  const std::size_t length = httpd_req_get_hdr_value_len(request, "Cookie");
  if (length == 0 || length > 512) return false;
  std::string cookies(length + 1, '\0');
  if (httpd_req_get_hdr_value_str(request, "Cookie", cookies.data(),
                                  cookies.size()) != ESP_OK) return false;
  cookies.resize(length);
  const std::string prefix = std::string(name) + "=";
  std::size_t start = 0;
  while (start < cookies.size()) {
    while (start < cookies.size() && cookies[start] == ' ') ++start;
    const std::size_t end = cookies.find(';', start);
    const std::string item = cookies.substr(start, end - start);
    if (item.rfind(prefix, 0) == 0) {
      *value = item.substr(prefix.size());
      return true;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

bool authenticated(httpd_req_t* request, bool require_csrf = false,
                   bool refresh_activity = true) {
  const std::int64_t now = esp_timer_get_time();
  if (!session.active || now - session.last_seen_us > kSessionIdleUs ||
      now - session.created_us > kSessionAbsoluteUs) {
    session = {};
    return false;
  }
  std::string token;
  if (!request_cookie(request, "gate_session", &token) ||
      !constant_time_equal(token, session.token)) return false;
  if (require_csrf) {
    const std::size_t length =
        httpd_req_get_hdr_value_len(request, "X-CSRF-Token");
    if (length == 0 || length > 128) return false;
    std::string csrf(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(request, "X-CSRF-Token", csrf.data(),
                                    csrf.size()) != ESP_OK) return false;
    csrf.resize(length);
    if (!constant_time_equal(csrf, session.csrf)) return false;
  }
  if (refresh_activity) session.last_seen_us = now;
  return true;
}

esp_err_t root_handler(httpd_req_t* request) {
  return send_asset(request, "text/html; charset=utf-8", index_html_start,
                    index_html_end, false);
}

esp_err_t javascript_handler(httpd_req_t* request) {
  return send_asset(request, "text/javascript; charset=utf-8", app_js_start,
                    app_js_end, false);
}

esp_err_t stylesheet_handler(httpd_req_t* request) {
  return send_asset(request, "text/css; charset=utf-8", app_css_start,
                    app_css_end, false);
}

esp_err_t status_handler(httpd_req_t* request) {
  const std::string response =
      "{\"provisioned\":" + std::string(application_provisioned ? "true" : "false") +
      ",\"connected\":" + std::string(station_connected ? "true" : "false") +
      ",\"hasWifi\":" + std::string(credentials.station_ssid[0] ? "true" : "false") +
      ",\"ssid\":\"" + json_escape(credentials.station_ssid) +
      "\",\"apSsid\":\"" + json_escape(access_point_ssid) + "\"}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t login_handler(httpd_req_t* request) {
  if (!application_provisioned) {
    return send_error(request, "409 Conflict", "Device is not provisioned.");
  }
  if (request->content_len <= 0 || request->content_len > 256) {
    return send_error(request, "400 Bad Request", "Invalid login request.");
  }
  std::string body(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (count <= 0) return ESP_FAIL;
    received += static_cast<std::size_t>(count);
  }
  std::string password;
  if (!form_value(body, "password", &password) ||
      !gate::config::verify_admin_password(password, active_config.admin)) {
    std::fill(password.begin(), password.end(), '\0');
    vTaskDelay(pdMS_TO_TICKS(500));
    return send_error(request, "401 Unauthorized", "Incorrect password.");
  }
  std::fill(password.begin(), password.end(), '\0');
  const std::int64_t now = esp_timer_get_time();
  session = {true, random_hex(32), random_hex(24), now, now};
  const std::string cookie = "gate_session=" + session.token +
      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800";
  httpd_resp_set_hdr(request, "Set-Cookie", cookie.c_str());
  httpd_resp_set_type(request, "application/json");
  const std::string response = "{\"csrf\":\"" + session.csrf + "\"}";
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t session_handler(httpd_req_t* request) {
  if (!authenticated(request)) {
    return send_error(request, "401 Unauthorized", "Authentication required.");
  }
  const std::string response = "{\"authenticated\":true,\"csrf\":\"" +
                               session.csrf + "\"}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t config_handler(httpd_req_t* request) {
  if (!authenticated(request)) {
    return send_error(request, "401 Unauthorized", "Authentication required.");
  }
  const std::string response =
      "{\"displayName\":\"" + json_escape(active_config.homekit.display_name.c_str()) +
      "\",\"ssid\":\"" + json_escape(active_config.wifi.ssid.c_str()) +
      "\",\"relayGpio\":" + std::to_string(active_config.relay.gpio) +
      ",\"relayActiveHigh\":" +
      std::string(active_config.relay.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"pulseMs\":" + std::to_string(active_config.relay.pulse_ms) +
      ",\"sensorGpio\":" + std::to_string(active_config.sensor.gpio) +
      ",\"sensorActiveHigh\":" +
      std::string(active_config.sensor.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"sensorPull\":\"" +
      std::string(active_config.sensor.pull == gate::config::SensorPull::kNone ? "none" :
                   active_config.sensor.pull == gate::config::SensorPull::kDown ? "down" : "up") + "\"" +
      ",\"feedbackActiveEndpoint\":\"" +
      std::string(active_config.sensor.active_endpoint ==
                          gate::config::FeedbackEndpoint::kOpen
                      ? "open" : "closed") + "\"" +
      ",\"feedbackStabilityMs\":" +
      std::to_string(active_config.sensor.endpoint_stability_ms) +
      ",\"openingSeconds\":" + std::to_string(active_config.timing.opening_ms / 1000) +
      ",\"closingSeconds\":" + std::to_string(active_config.timing.closing_ms / 1000) +
      ",\"hardwareMonitoring\":" +
      std::string(gate::hardware::monitoring_active() ? "true" : "false") +
      ",\"feedbackActive\":" +
      std::string(gate::hardware::feedback_active() ? "true" : "false") +
      ",\"relayControlEnabled\":" +
      std::string(gate::runtime::active() ? "true" : "false") + "}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t runtime_handler(httpd_req_t* request) {
  if (!authenticated(request, false, false)) {
    return send_error(request, "401 Unauthorized", "Authentication required.");
  }
  const std::string response =
      "{\"hardwareMonitoring\":" +
      std::string(gate::hardware::monitoring_active() ? "true" : "false") +
      ",\"feedbackActive\":" +
      std::string(gate::hardware::feedback_active() ? "true" : "false") +
      ",\"relayControlEnabled\":" +
      std::string(gate::runtime::active() ? "true" : "false") + "}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t relay_pulse_handler(httpd_req_t* request) {
  if (!authenticated(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  switch (gate::runtime::request_bench_pulse()) {
    case gate::runtime::RequestResult::kAccepted:
      httpd_resp_set_status(request, "202 Accepted");
      httpd_resp_set_type(request, "application/json");
      return httpd_resp_sendstr(request, "{\"accepted\":true}");
    case gate::runtime::RequestResult::kBusy:
      return send_error(request, "409 Conflict",
                        "Relay pulse is active or cooling down. Try again shortly.");
    case gate::runtime::RequestResult::kHardwareError:
      return send_error(request, "500 Internal Server Error",
                        "Relay GPIO activation failed.");
    case gate::runtime::RequestResult::kUnavailable:
      return send_error(request, "503 Service Unavailable",
                        "Gate runtime is not available.");
  }
  return ESP_FAIL;
}

esp_err_t homekit_handler(httpd_req_t* request) {
  if (!authenticated(request)) {
    return send_error(request, "401 Unauthorized", "Authentication required.");
  }
  const std::string& code = active_config.homekit.setup_code;
  const std::string formatted_code =
      code.size() == 8 ? code.substr(0, 3) + "-" + code.substr(3, 2) + "-" +
                             code.substr(5, 3)
                       : code;
  const std::string response =
      "{\"active\":" +
      std::string(gate::homekit::active() ? "true" : "false") +
      ",\"paired\":" +
      std::string(gate::homekit::paired() ? "true" : "false") +
      ",\"setupCode\":\"" + json_escape(formatted_code.c_str()) +
      "\",\"setupId\":\"" +
      json_escape(active_config.homekit.setup_id.c_str()) + "\"}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t update_config_handler(httpd_req_t* request) {
  if (!authenticated(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  if (request->content_len <= 0 || request->content_len > 1024) {
    return send_error(request, "400 Bad Request", "Invalid settings request.");
  }
  std::string body(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (count <= 0) return ESP_FAIL;
    received += static_cast<std::size_t>(count);
  }

  std::string display_name;
  std::string relay_level;
  std::string sensor_level;
  std::string sensor_pull;
  std::string feedback_endpoint;
  int relay_gpio = -1;
  int sensor_gpio = -1;
  int pulse_ms = 0;
  int opening_seconds = 0;
  int closing_seconds = 0;
  int feedback_stability_ms = 0;
  if (!form_value(body, "displayName", &display_name) || display_name.empty() ||
      display_name.size() > 64 ||
      !form_value(body, "relayLevel", &relay_level) ||
      !form_value(body, "sensorLevel", &sensor_level) ||
      !form_value(body, "sensorPull", &sensor_pull) ||
      !form_value(body, "feedbackEndpoint", &feedback_endpoint) ||
      !parse_integer(body, "relayGpio", 0, 39, &relay_gpio) ||
      !parse_integer(body, "sensorGpio", 0, 39, &sensor_gpio) ||
      !parse_integer(body, "pulseMs", 100, 2000, &pulse_ms) ||
      !parse_integer(body, "openingSeconds", 3, 180, &opening_seconds) ||
      !parse_integer(body, "closingSeconds", 3, 180, &closing_seconds) ||
      !parse_integer(body, "feedbackStabilityMs", 1000, 10000,
                     &feedback_stability_ms) ||
      (relay_level != "low" && relay_level != "high") ||
      (sensor_level != "low" && sensor_level != "high") ||
      (sensor_pull != "none" && sensor_pull != "up" && sensor_pull != "down") ||
      (feedback_endpoint != "open" && feedback_endpoint != "closed")) {
    return send_error(request, "400 Bad Request",
                      "One or more settings have an invalid format.");
  }

  gate::config::AppConfig updated = active_config;
  updated.homekit.display_name = display_name;
  updated.relay.gpio = relay_gpio;
  updated.relay.active_level = parse_level(relay_level);
  updated.relay.pulse_ms = pulse_ms;
  updated.sensor.gpio = sensor_gpio;
  updated.sensor.active_level = parse_level(sensor_level);
  updated.sensor.pull = parse_pull(sensor_pull);
  updated.sensor.active_endpoint =
      feedback_endpoint == "open" ? gate::config::FeedbackEndpoint::kOpen
                                  : gate::config::FeedbackEndpoint::kClosed;
  updated.sensor.endpoint_stability_ms = feedback_stability_ms;
  updated.timing.opening_ms = opening_seconds * 1000;
  updated.timing.closing_ms = closing_seconds * 1000;
  const auto errors = gate::config::validate(updated);
  if (!errors.empty()) {
    return send_error(request, "400 Bad Request",
                      errors.front().field + ": " + errors.front().message);
  }
  const esp_err_t result = gate::config::ConfigRepository().save(updated);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Failed to save authenticated settings: %s",
             esp_err_to_name(result));
    return send_error(request, "500 Internal Server Error",
                      "Could not persist settings.");
  }
  active_config = std::move(updated);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"saved\":true}");
}

esp_err_t password_change_handler(httpd_req_t* request) {
  if (!authenticated(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  if (request->content_len <= 0 || request->content_len > 512) {
    return send_error(request, "400 Bad Request", "Invalid password request.");
  }
  std::string body(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (count <= 0) return ESP_FAIL;
    received += static_cast<std::size_t>(count);
  }

  std::string current_password;
  std::string new_password;
  std::string confirmation;
  const bool valid_format =
      form_value(body, "currentPassword", &current_password) &&
      form_value(body, "newPassword", &new_password) &&
      form_value(body, "confirmation", &confirmation) &&
      new_password.size() >= 10 && new_password.size() <= 128 &&
      new_password == confirmation;
  std::fill(confirmation.begin(), confirmation.end(), '\0');
  if (!valid_format) {
    std::fill(current_password.begin(), current_password.end(), '\0');
    std::fill(new_password.begin(), new_password.end(), '\0');
    return send_error(request, "400 Bad Request",
                      "New passwords must match and contain 10-128 characters.");
  }
  if (!gate::config::verify_admin_password(current_password,
                                            active_config.admin)) {
    std::fill(current_password.begin(), current_password.end(), '\0');
    std::fill(new_password.begin(), new_password.end(), '\0');
    vTaskDelay(pdMS_TO_TICKS(500));
    return send_error(request, "401 Unauthorized",
                      "Current administrator password is incorrect.");
  }
  std::fill(current_password.begin(), current_password.end(), '\0');

  gate::config::AppConfig updated = active_config;
  esp_err_t result = gate::config::derive_admin_password(new_password,
                                                          &updated.admin);
  std::fill(new_password.begin(), new_password.end(), '\0');
  if (result != ESP_OK) {
    return send_error(request, "500 Internal Server Error",
                      "Could not derive new administrator credentials.");
  }
  result = gate::config::ConfigRepository().save(updated);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Failed to save administrator password: %s",
             esp_err_to_name(result));
    return send_error(request, "500 Internal Server Error",
                      "Could not persist the new password.");
  }

  active_config = std::move(updated);
  session = {};
  httpd_resp_set_hdr(request, "Set-Cookie",
                     "gate_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"changed\":true}");
}

esp_err_t wifi_change_handler(httpd_req_t* request) {
  if (!authenticated(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  if (request->content_len <= 0 || request->content_len > 512) {
    return send_error(request, "400 Bad Request", "Invalid network request.");
  }
  std::string body(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (count <= 0) return ESP_FAIL;
    received += static_cast<std::size_t>(count);
  }

  std::string ssid;
  std::string wifi_password;
  std::string admin_password;
  const bool valid_format =
      form_value(body, "ssid", &ssid) &&
      form_value(body, "wifiPassword", &wifi_password) &&
      form_value(body, "adminPassword", &admin_password) &&
      !ssid.empty() && ssid.size() <= 32 && wifi_password.size() <= 63 &&
      (wifi_password.empty() || wifi_password.size() >= 8);
  if (!valid_format) {
    std::fill(wifi_password.begin(), wifi_password.end(), '\0');
    std::fill(admin_password.begin(), admin_password.end(), '\0');
    return send_error(request, "400 Bad Request",
                      "Enter an SSID and either no password or 8-63 characters.");
  }
  if (!gate::config::verify_admin_password(admin_password,
                                            active_config.admin)) {
    std::fill(wifi_password.begin(), wifi_password.end(), '\0');
    std::fill(admin_password.begin(), admin_password.end(), '\0');
    vTaskDelay(pdMS_TO_TICKS(500));
    return send_error(request, "401 Unauthorized",
                      "Administrator password is incorrect.");
  }
  std::fill(admin_password.begin(), admin_password.end(), '\0');

  gate::config::AppConfig previous = active_config;
  gate::config::AppConfig updated = active_config;
  updated.wifi.ssid = ssid;
  updated.wifi.password = wifi_password;
  const auto errors = gate::config::validate(updated);
  if (!errors.empty()) {
    std::fill(wifi_password.begin(), wifi_password.end(), '\0');
    return send_error(request, "400 Bad Request",
                      errors.front().field + ": " + errors.front().message);
  }

  esp_err_t result = gate::config::ConfigRepository().save(updated);
  if (result != ESP_OK) {
    std::fill(wifi_password.begin(), wifi_password.end(), '\0');
    return send_error(request, "500 Internal Server Error",
                      "Could not persist the network configuration.");
  }

  const BootstrapCredentials previous_credentials = credentials;
  copy_string(credentials.station_ssid, ssid);
  copy_string(credentials.station_password, wifi_password);
  std::fill(wifi_password.begin(), wifi_password.end(), '\0');
  result = save_credentials();
  if (result != ESP_OK) {
    credentials = previous_credentials;
    const esp_err_t rollback = gate::config::ConfigRepository().save(previous);
    if (rollback != ESP_OK) {
      ESP_LOGE(kTag, "Wi-Fi update rollback failed: %s",
               esp_err_to_name(rollback));
    }
    return send_error(request, "500 Internal Server Error",
                      "Could not persist bootstrap network credentials.");
  }

  active_config = std::move(updated);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_sendstr(request, "{\"saved\":true,\"restarting\":true}");
  xTaskCreate(restart_task, "network_restart", 2048, nullptr, 5, nullptr);
  return ESP_OK;
}

esp_err_t logout_handler(httpd_req_t* request) {
  if (!authenticated(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  session = {};
  httpd_resp_set_hdr(request, "Set-Cookie",
                     "gate_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  return httpd_resp_sendstr(request, "{}");
}

esp_err_t reboot_handler(httpd_req_t* request) {
  if (!authenticated(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  httpd_resp_set_type(request, "application/json");
  httpd_resp_sendstr(request, "{\"restarting\":true}");
  xTaskCreate(restart_task, "admin_restart", 2048, nullptr, 5, nullptr);
  return ESP_OK;
}

void restart_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(1200));
  esp_restart();
}

void dns_task(void*) {
  const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) {
    ESP_LOGE(kTag, "Could not create captive DNS socket");
    vTaskDelete(nullptr);
    return;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(53);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    ESP_LOGE(kTag, "Could not bind captive DNS socket");
    close(socket_fd);
    vTaskDelete(nullptr);
    return;
  }

  std::array<std::uint8_t, 512> packet{};
  while (true) {
    sockaddr_in client{};
    socklen_t client_length = sizeof(client);
    const int length = recvfrom(socket_fd, packet.data(), packet.size(), 0,
                                reinterpret_cast<sockaddr*>(&client),
                                &client_length);
    if (length < 12) continue;

    std::size_t question_end = 12;
    while (question_end < static_cast<std::size_t>(length) &&
           packet[question_end] != 0) {
      const std::size_t label_length = packet[question_end];
      if (label_length > 63 || question_end + label_length + 1 >=
                                   static_cast<std::size_t>(length)) {
        question_end = packet.size();
        break;
      }
      question_end += label_length + 1;
    }
    if (question_end + 5 > static_cast<std::size_t>(length)) continue;
    question_end += 5;  // root label, QTYPE, and QCLASS.
    if (question_end + 16 > packet.size()) continue;

    packet[2] = 0x81;
    packet[3] = 0x80;
    packet[6] = 0;
    packet[7] = 1;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    std::size_t response_length = question_end;
    const std::uint8_t answer[] = {
        0xC0, 0x0C,  // Pointer to queried name.
        0x00, 0x01,  // A record.
        0x00, 0x01,  // Internet class.
        0x00, 0x00, 0x00, 0x3C,  // 60-second TTL.
        0x00, 0x04, 192, 168, 4, 1};
    std::memcpy(packet.data() + response_length, answer, sizeof(answer));
    response_length += sizeof(answer);
    sendto(socket_fd, packet.data(), response_length, 0,
           reinterpret_cast<sockaddr*>(&client), client_length);
  }
}

esp_err_t save_handler(httpd_req_t* request) {
  if (application_provisioned) {
    return send_error(request, "403 Forbidden", "Configuration is already saved.");
  }
  if (request->content_len <= 0 || request->content_len > kMaxRequestBody) {
    return send_error(request, "400 Bad Request", "Invalid request size.");
  }
  std::string body(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (count <= 0) return ESP_FAIL;
    received += static_cast<std::size_t>(count);
  }

  std::string ssid;
  std::string password;
  std::string admin_password;
  std::string display_name;
  std::string setup_code;
  std::string setup_id;
  std::string relay_level;
  std::string sensor_level;
  std::string sensor_pull;
  std::string feedback_endpoint;
  int relay_gpio = -1;
  int sensor_gpio = -1;
  int pulse_ms = 0;
  int opening_seconds = 0;
  int closing_seconds = 0;
  int feedback_stability_ms = 0;
  const bool submitted_wifi = form_value(body, "ssid", &ssid);
  if (submitted_wifi) form_value(body, "password", &password);
  if (!submitted_wifi && credentials.station_ssid[0] != '\0') {
    ssid = credentials.station_ssid;
    password = credentials.station_password;
  }
  if (ssid.empty() || ssid.size() > 32 || password.size() > 63 ||
      (!password.empty() && password.size() < 8)) {
    return send_error(request, "400 Bad Request",
                      "Enter an SSID and either no password or 8-63 characters.");
  }

  const bool fields_valid =
      form_value(body, "adminPassword", &admin_password) &&
      form_value(body, "displayName", &display_name) &&
      form_value(body, "setupCode", &setup_code) &&
      form_value(body, "setupId", &setup_id) &&
      form_value(body, "relayLevel", &relay_level) &&
      form_value(body, "sensorLevel", &sensor_level) &&
      form_value(body, "sensorPull", &sensor_pull) &&
      form_value(body, "feedbackEndpoint", &feedback_endpoint) &&
      parse_integer(body, "relayGpio", 0, 39, &relay_gpio) &&
      parse_integer(body, "sensorGpio", 0, 39, &sensor_gpio) &&
      parse_integer(body, "pulseMs", 100, 2000, &pulse_ms) &&
      parse_integer(body, "openingSeconds", 3, 180, &opening_seconds) &&
      parse_integer(body, "closingSeconds", 3, 180, &closing_seconds) &&
      parse_integer(body, "feedbackStabilityMs", 1000, 10000,
                    &feedback_stability_ms) &&
      admin_password.size() >= 10 && admin_password.size() <= 128 &&
      (relay_level == "low" || relay_level == "high") &&
      (sensor_level == "low" || sensor_level == "high") &&
      (sensor_pull == "none" || sensor_pull == "up" || sensor_pull == "down") &&
      (feedback_endpoint == "open" || feedback_endpoint == "closed");
  if (!fields_valid) {
    return send_error(request, "400 Bad Request",
                      "One or more setup fields have an invalid format.");
  }

  gate::config::AppConfig app_config;
  app_config.wifi.ssid = ssid;
  app_config.wifi.password = password;
  app_config.access_point.password = credentials.ap_password;
  app_config.homekit.display_name = display_name;
  app_config.homekit.setup_code = setup_code;
  app_config.homekit.setup_id = setup_id;
  app_config.relay.gpio = relay_gpio;
  app_config.relay.active_level = parse_level(relay_level);
  app_config.relay.pulse_ms = pulse_ms;
  app_config.sensor.gpio = sensor_gpio;
  app_config.sensor.active_level = parse_level(sensor_level);
  app_config.sensor.pull = parse_pull(sensor_pull);
  app_config.sensor.active_endpoint =
      feedback_endpoint == "open" ? gate::config::FeedbackEndpoint::kOpen
                                  : gate::config::FeedbackEndpoint::kClosed;
  app_config.sensor.endpoint_stability_ms = feedback_stability_ms;
  app_config.timing.opening_ms = opening_seconds * 1000;
  app_config.timing.closing_ms = closing_seconds * 1000;
  esp_err_t result = gate::config::derive_admin_password(admin_password,
                                                          &app_config.admin);
  std::fill(admin_password.begin(), admin_password.end(), '\0');
  if (result != ESP_OK) {
    return send_error(request, "500 Internal Server Error",
                      "Could not derive administrator credentials.");
  }
  const auto validation_errors = gate::config::validate(app_config);
  if (!validation_errors.empty()) {
    const std::string message = validation_errors.front().field + ": " +
                                validation_errors.front().message;
    return send_error(request, "400 Bad Request", message);
  }

  copy_string(credentials.station_ssid, ssid);
  copy_string(credentials.station_password, password);
  result = save_credentials();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Failed to save Wi-Fi credentials: %s",
             esp_err_to_name(result));
    return send_error(request, "500 Internal Server Error",
                      "Could not save settings.");
  }
  result = gate::config::ConfigRepository().save(app_config);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Failed to save application configuration: %s",
             esp_err_to_name(result));
    return send_error(request, "500 Internal Server Error",
                      "Could not save complete configuration.");
  }
  application_provisioned = true;

  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_sendstr(request,
      "<!doctype html><meta name=viewport content='width=device-width'>"
      "<h1>Saved</h1><p>The controller is restarting and will try your Wi-Fi. "
      "Reconnect to the setup network if needed.</p>");
  xTaskCreate(restart_task, "provision_restart", 2048, nullptr, 5, nullptr);
  return ESP_OK;
}

esp_err_t captive_handler(httpd_req_t* request) {
  httpd_resp_set_status(request, "302 Found");
  httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
  httpd_resp_send(request, nullptr, 0);
  return ESP_OK;
}

void wifi_event(arduino_event_id_t event, arduino_event_info_t) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED &&
      credentials.station_ssid[0] != '\0') {
    station_connected = false;
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    station_connected = true;
    ESP_LOGI(kTag, "Station connected; setup AP remains active");
  }
}

esp_err_t start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  // Sixteen routes are currently registered. Keep a little headroom so adding
  // the next HomeKit-management endpoint cannot silently exhaust the table.
  config.max_uri_handlers = 20;
  esp_err_t result = httpd_start(&server, &config);
  if (result != ESP_OK) return result;

  const httpd_uri_t root{.uri = "/", .method = HTTP_GET,
                         .handler = root_handler, .user_ctx = nullptr};
  const httpd_uri_t javascript{.uri = "/app.js", .method = HTTP_GET,
                               .handler = javascript_handler, .user_ctx = nullptr};
  const httpd_uri_t stylesheet{.uri = "/app.css", .method = HTTP_GET,
                               .handler = stylesheet_handler, .user_ctx = nullptr};
  const httpd_uri_t status{.uri = "/api/v1/setup/status", .method = HTTP_GET,
                           .handler = status_handler, .user_ctx = nullptr};
  const httpd_uri_t login{.uri = "/api/v1/session/login", .method = HTTP_POST,
                          .handler = login_handler, .user_ctx = nullptr};
  const httpd_uri_t session_status{.uri = "/api/v1/session", .method = HTTP_GET,
                                   .handler = session_handler, .user_ctx = nullptr};
  const httpd_uri_t config_api{.uri = "/api/v1/config", .method = HTTP_GET,
                               .handler = config_handler, .user_ctx = nullptr};
  const httpd_uri_t runtime_api{.uri = "/api/v1/runtime", .method = HTTP_GET,
                                 .handler = runtime_handler, .user_ctx = nullptr};
  const httpd_uri_t relay_pulse{
      .uri = "/api/v1/gate/test-pulse", .method = HTTP_POST,
      .handler = relay_pulse_handler, .user_ctx = nullptr};
  const httpd_uri_t homekit_api{
      .uri = "/api/v1/homekit", .method = HTTP_GET,
      .handler = homekit_handler, .user_ctx = nullptr};
  const httpd_uri_t update_config{.uri = "/api/v1/config", .method = HTTP_PUT,
                                  .handler = update_config_handler, .user_ctx = nullptr};
  const httpd_uri_t change_password{
      .uri = "/api/v1/access/password", .method = HTTP_PUT,
      .handler = password_change_handler, .user_ctx = nullptr};
  const httpd_uri_t change_wifi{
      .uri = "/api/v1/network/wifi", .method = HTTP_PUT,
      .handler = wifi_change_handler, .user_ctx = nullptr};
  const httpd_uri_t logout{.uri = "/api/v1/session/logout", .method = HTTP_POST,
                           .handler = logout_handler, .user_ctx = nullptr};
  const httpd_uri_t reboot{.uri = "/api/v1/system/reboot", .method = HTTP_POST,
                           .handler = reboot_handler, .user_ctx = nullptr};
  const httpd_uri_t save{.uri = "/save", .method = HTTP_POST,
                         .handler = save_handler, .user_ctx = nullptr};
  const httpd_uri_t captive{.uri = "/*", .method = HTTP_GET,
                            .handler = captive_handler, .user_ctx = nullptr};
  if ((result = httpd_register_uri_handler(server, &root)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &javascript)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &stylesheet)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &status)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &login)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &session_status)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &config_api)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &runtime_api)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &relay_pulse)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &homekit_api)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &update_config)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &change_password)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &change_wifi)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &logout)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &reboot)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &save)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &captive)) != ESP_OK) {
    httpd_stop(server);
    server = nullptr;
  }
  return result;
}

}  // namespace

esp_err_t start() {
  esp_err_t result = load_or_create_credentials();
  if (result != ESP_OK) return result;

  gate::config::AppConfig saved_config;
  application_provisioned =
      gate::config::ConfigRepository().load(&saved_config) == ESP_OK;
  if (application_provisioned) active_config = std::move(saved_config);

  std::uint8_t mac[6]{};
  ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), kTag,
                      "Could not read device MAC");
  std::snprintf(access_point_ssid, sizeof(access_point_ssid),
                "GateSetup-%02X%02X%02X", mac[3], mac[4], mac[5]);

  WiFi.onEvent(wifi_event);
  if (!WiFi.mode(WIFI_AP_STA)) {
    ESP_LOGE(kTag, "Could not set Arduino Wi-Fi AP+STA mode");
    return ESP_FAIL;
  }
  if (!WiFi.softAP(access_point_ssid, credentials.ap_password, 1, 0, 4)) {
    ESP_LOGE(kTag, "Could not configure Arduino setup AP");
    return ESP_FAIL;
  }
  // HomeSpan is the sole owner of station connect/reconnect. It receives the
  // same persisted credentials in homekit::start() and calls WiFi.begin()
  // after its polling task is ready to consume the resulting network events.
  ESP_RETURN_ON_ERROR(start_http_server(), kTag,
                      "Could not start setup web server");
  if (xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 4, nullptr) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(kTag, "Setup AP SSID: %s", access_point_ssid);
  ESP_LOGI(kTag, "Setup AP password: %s", credentials.ap_password);
  ESP_LOGI(kTag, "Setup URL: http://192.168.4.1/");
  return ESP_OK;
}

}  // namespace gate::provisioning
