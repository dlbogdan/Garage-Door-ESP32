#include "provisioning.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "app_config.hpp"
#include "bootstrap_credentials.hpp"
#include "captive_dns.hpp"
#include "config_repository.hpp"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gate_hardware.hpp"
#include "password.hpp"
#include "gate_runtime.hpp"
#include "homespan_compatibility.hpp"
#include "network_manager.hpp"

namespace gate::provisioning {
namespace {

constexpr char kTag[] = "provisioning";
constexpr std::size_t kMaxRequestBody = 2048;

gate::bootstrap::Credentials credentials;
httpd_handle_t server = nullptr;
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
      ",\"connected\":" + std::string(gate::network::station_connected() ? "true" : "false") +
      ",\"hasWifi\":" + std::string(credentials.station_ssid[0] ? "true" : "false") +
      ",\"ssid\":\"" + json_escape(credentials.station_ssid) +
      "\",\"apSsid\":\"" + json_escape(gate::network::access_point_ssid()) + "\"}";
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
  const auto& op = active_config.gate_operator;
  const bool directional = op.profile == gate::config::OperatorProfile::kDirectional;
  const bool dual = op.feedback_topology == gate::config::FeedbackTopology::kDual;
  const auto& relay = directional ? op.open : op.step;
  const auto& sensor = dual ? op.opened_feedback : op.single_feedback;
  const std::string response =
      "{\"displayName\":\"" + json_escape(active_config.homekit.display_name.c_str()) +
      "\",\"ssid\":\"" + json_escape(active_config.wifi.ssid.c_str()) +
      "\",\"schemaVersion\":3" +
      ",\"operatorProfile\":\"" + std::string(directional ? "directional" : "sequential") + "\"" +
      ",\"feedbackMode\":\"" + std::string(dual ? "dual" : "single") + "\"" +
      ",\"legacyFlatConfigLossy\":" + std::string(directional || dual ? "true" : "false") +
      ",\"relayGpio\":" + std::to_string(relay.gpio) +
      ",\"relayActiveHigh\":" +
      std::string(relay.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"pulseMs\":" + std::to_string(relay.pulse_ms) +
      ",\"openGpio\":" + std::to_string(op.open.gpio) +
      ",\"openActiveHigh\":" + std::string(op.open.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"openPulseMs\":" + std::to_string(op.open.pulse_ms) +
      ",\"closeGpio\":" + std::to_string(op.close.gpio) +
      ",\"closeActiveHigh\":" + std::string(op.close.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"closePulseMs\":" + std::to_string(op.close.pulse_ms) +
      ",\"sensorGpio\":" + std::to_string(sensor.gpio) +
      ",\"sensorActiveHigh\":" +
      std::string(sensor.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"sensorPull\":\"" +
      std::string(sensor.pull == gate::config::SensorPull::kNone ? "none" :
                   sensor.pull == gate::config::SensorPull::kDown ? "down" : "up") + "\"" +
      ",\"feedbackActiveEndpoint\":\"" +
      std::string(op.single_active_endpoint ==
                          gate::config::FeedbackEndpoint::kOpen
                      ? "open" : "closed") + "\"" +
      ",\"feedbackStabilityMs\":" +
      std::to_string(op.endpoint_stability_ms) +
      ",\"openingSeconds\":" + std::to_string(active_config.timing.opening_ms / 1000) +
      ",\"closingSeconds\":" + std::to_string(active_config.timing.closing_ms / 1000) +
      ",\"hardwareMonitoring\":" +
      std::string(gate::hardware::monitoring_active() ? "true" : "false") +
      ",\"feedbackActive\":" +
      std::string((gate::hardware::feedback_assertions().opened || gate::hardware::feedback_assertions().closed) ? "true" : "false") +
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
  const auto state = gate::runtime::snapshot();
  const auto assertions = gate::hardware::feedback_assertions();
  const std::string response =
      "{\"hardwareMonitoring\":" +
      std::string(gate::hardware::monitoring_active() ? "true" : "false") +
      ",\"feedbackActive\":" +
      std::string((assertions.opened || assertions.closed) ? "true" : "false") +
      ",\"openedAsserted\":" + std::string(assertions.opened ? "true" : "false") +
      ",\"closedAsserted\":" + std::string(assertions.closed ? "true" : "false") +
      ",\"observation\":\"" + std::string(state.observation_valid ? gate::controller::to_string(state.observation) : "UNKNOWN") + "\"" +
      ",\"state\":\"" + std::string(gate::controller::to_string(state.state)) + "\"" +
      ",\"target\":\"" + std::string(gate::controller::to_string(state.target)) + "\"" +
      ",\"activeCommand\":\"" + std::string(gate::controller::to_string(state.active_command)) + "\"" +
      ",\"pulseActive\":" + std::string(state.pulse_active ? "true" : "false") +
      ",\"obstruction\":" + std::string(state.obstruction ? "true" : "false") +
      ",\"faultReason\":\"" + std::string(gate::controller::to_string(state.fault)) + "\"" +
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
  std::string operator_profile;
  std::string feedback_mode;
  const bool has_operator_profile =
      form_value(body, "operatorProfile", &operator_profile);
  const bool has_feedback_mode = form_value(body, "feedbackMode", &feedback_mode);
  const bool canonical = has_operator_profile && has_feedback_mode;
  if (has_operator_profile != has_feedback_mode) {
    return send_error(request, "400 Bad Request",
                      "Operator profile and feedback topology must both be provided.");
  }
  if (!form_value(body, "displayName", &display_name) || display_name.empty() ||
      display_name.size() > 64 ||
      !parse_integer(body, "openingSeconds", 3, 180, &opening_seconds) ||
      !parse_integer(body, "closingSeconds", 3, 180, &closing_seconds) ||
      !parse_integer(body, "feedbackStabilityMs", 1000, 10000,
                     &feedback_stability_ms)) {
    return send_error(request, "400 Bad Request",
                      "One or more settings have an invalid format.");
  }

  gate::config::AppConfig updated = active_config;
  updated.homekit.display_name = display_name;
  if (!canonical) {
    if (!form_value(body, "relayLevel", &relay_level) ||
        !form_value(body, "sensorLevel", &sensor_level) ||
        !form_value(body, "sensorPull", &sensor_pull) ||
        !form_value(body, "feedbackEndpoint", &feedback_endpoint) ||
        !parse_integer(body, "relayGpio", 0, 39, &relay_gpio) ||
        !parse_integer(body, "sensorGpio", 0, 39, &sensor_gpio) ||
        !parse_integer(body, "pulseMs", 100, 2000, &pulse_ms) ||
        (relay_level != "low" && relay_level != "high") ||
        (sensor_level != "low" && sensor_level != "high") ||
        (sensor_pull != "none" && sensor_pull != "up" && sensor_pull != "down") ||
        (feedback_endpoint != "open" && feedback_endpoint != "closed")) {
      return send_error(request, "400 Bad Request", "Invalid legacy gate settings.");
    }
    updated.gate_operator.profile = gate::config::OperatorProfile::kSequential;
    updated.gate_operator.feedback_topology = gate::config::FeedbackTopology::kSingle;
    updated.gate_operator.step = {relay_gpio, parse_level(relay_level),
                                  static_cast<std::uint32_t>(pulse_ms)};
    updated.gate_operator.single_feedback = {
        sensor_gpio, parse_level(sensor_level), parse_pull(sensor_pull), 50};
    updated.gate_operator.single_active_endpoint =
        feedback_endpoint == "open" ? gate::config::FeedbackEndpoint::kOpen
                                    : gate::config::FeedbackEndpoint::kClosed;
  } else {
    if ((operator_profile != "sequential" && operator_profile != "directional") ||
        (feedback_mode != "single" && feedback_mode != "dual")) {
      return send_error(request, "400 Bad Request", "Invalid operator discriminators.");
    }
    updated.gate_operator.profile = operator_profile == "directional"
        ? gate::config::OperatorProfile::kDirectional
        : gate::config::OperatorProfile::kSequential;
    updated.gate_operator.feedback_topology = feedback_mode == "dual"
        ? gate::config::FeedbackTopology::kDual
        : gate::config::FeedbackTopology::kSingle;

    auto parse_output = [&](const char* gpio_name, const char* level_name,
                            const char* pulse_name,
                            gate::config::PulseOutputConfig* output) {
      std::string level;
      int gpio = -1;
      int pulse = 0;
      if (!parse_integer(body, gpio_name, 0, 39, &gpio) ||
          !form_value(body, level_name, &level) ||
          !parse_integer(body, pulse_name, 100, 2000, &pulse) ||
          (level != "low" && level != "high")) return false;
      *output = {gpio, parse_level(level), static_cast<std::uint32_t>(pulse)};
      return true;
    };
    if (updated.gate_operator.profile == gate::config::OperatorProfile::kSequential) {
      if (!parse_output("stepGpio", "stepLevel", "stepPulseMs",
                        &updated.gate_operator.step)) {
        return send_error(request, "400 Bad Request", "Invalid STEP output.");
      }
    } else if (!parse_output("openGpio", "openLevel", "openPulseMs",
                             &updated.gate_operator.open) ||
               !parse_output("closeGpio", "closeLevel", "closePulseMs",
                             &updated.gate_operator.close)) {
      return send_error(request, "400 Bad Request", "Invalid directional outputs.");
    }

    auto parse_input = [&](const char* gpio_name, const char* level_name,
                           const char* pull_name,
                           gate::config::FeedbackInputConfig* input) {
      std::string level;
      std::string pull;
      int gpio = -1;
      if (!parse_integer(body, gpio_name, 0, 39, &gpio) ||
          !form_value(body, level_name, &level) ||
          !form_value(body, pull_name, &pull) ||
          (level != "low" && level != "high") ||
          (pull != "none" && pull != "up" && pull != "down")) return false;
      *input = {gpio, parse_level(level), parse_pull(pull), 50};
      return true;
    };
    if (updated.gate_operator.feedback_topology == gate::config::FeedbackTopology::kSingle) {
      if (!form_value(body, "feedbackEndpoint", &feedback_endpoint) ||
          (feedback_endpoint != "open" && feedback_endpoint != "closed") ||
          !parse_input("sensorGpio", "sensorLevel", "sensorPull",
                       &updated.gate_operator.single_feedback)) {
        return send_error(request, "400 Bad Request", "Invalid single feedback input.");
      }
      updated.gate_operator.single_active_endpoint =
          feedback_endpoint == "open" ? gate::config::FeedbackEndpoint::kOpen
                                      : gate::config::FeedbackEndpoint::kClosed;
    } else if (!parse_input("openedSensorGpio", "openedSensorLevel",
                            "openedSensorPull", &updated.gate_operator.opened_feedback) ||
               !parse_input("closedSensorGpio", "closedSensorLevel",
                            "closedSensorPull", &updated.gate_operator.closed_feedback)) {
      return send_error(request, "400 Bad Request", "Invalid dual feedback inputs.");
    }
  }
  updated.gate_operator.endpoint_stability_ms = feedback_stability_ms;
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

  const gate::bootstrap::Credentials previous_credentials = credentials;
  gate::bootstrap::set_station(&credentials, ssid, wifi_password);
  std::fill(wifi_password.begin(), wifi_password.end(), '\0');
  result = gate::bootstrap::save(credentials);
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

  if (!form_value(body, "adminPassword", &admin_password)) {
    return send_error(request, "400 Bad Request", "Administrator password is missing.");
  }
  if (admin_password.size() < 10 || admin_password.size() > 128) {
    return send_error(request, "400 Bad Request",
                      "Administrator password must contain 10-128 characters.");
  }
  if (!form_value(body, "displayName", &display_name) || display_name.empty()) {
    return send_error(request, "400 Bad Request", "Apple Home display name is missing.");
  }
  if (!form_value(body, "setupCode", &setup_code)) {
    return send_error(request, "400 Bad Request", "Apple Home setup code is missing.");
  }
  if (!form_value(body, "setupId", &setup_id)) {
    return send_error(request, "400 Bad Request", "Apple Home Setup ID is missing.");
  }
  if (!form_value(body, "relayLevel", &relay_level) ||
      (relay_level != "low" && relay_level != "high")) {
    return send_error(request, "400 Bad Request", "Relay active level is invalid.");
  }
  if (!form_value(body, "sensorLevel", &sensor_level) ||
      (sensor_level != "low" && sensor_level != "high")) {
    return send_error(request, "400 Bad Request", "Feedback active level is invalid.");
  }
  if (!form_value(body, "sensorPull", &sensor_pull) ||
      (sensor_pull != "none" && sensor_pull != "up" && sensor_pull != "down")) {
    return send_error(request, "400 Bad Request", "Feedback pull mode is invalid.");
  }
  if (!form_value(body, "feedbackEndpoint", &feedback_endpoint) ||
      (feedback_endpoint != "open" && feedback_endpoint != "closed")) {
    return send_error(request, "400 Bad Request", "Feedback endpoint meaning is invalid.");
  }
  if (!parse_integer(body, "relayGpio", 0, 39, &relay_gpio)) {
    return send_error(request, "400 Bad Request", "Relay GPIO must be between 0 and 39.");
  }
  if (!parse_integer(body, "sensorGpio", 0, 39, &sensor_gpio)) {
    return send_error(request, "400 Bad Request", "Feedback GPIO must be between 0 and 39.");
  }
  if (!parse_integer(body, "pulseMs", 100, 2000, &pulse_ms)) {
    return send_error(request, "400 Bad Request", "Pulse duration must be 100-2000 ms.");
  }
  if (!parse_integer(body, "openingSeconds", 3, 180, &opening_seconds)) {
    return send_error(request, "400 Bad Request", "Opening time must be 3-180 seconds.");
  }
  if (!parse_integer(body, "closingSeconds", 3, 180, &closing_seconds)) {
    return send_error(request, "400 Bad Request", "Closing time must be 3-180 seconds.");
  }
  if (!parse_integer(body, "feedbackStabilityMs", 1000, 10000,
                     &feedback_stability_ms)) {
    return send_error(request, "400 Bad Request",
                      "Endpoint stability must be 1000-10000 ms.");
  }

  gate::config::AppConfig app_config;
  app_config.wifi.ssid = ssid;
  app_config.wifi.password = password;
  app_config.access_point.password = credentials.ap_password;
  app_config.homekit.display_name = display_name;
  app_config.homekit.setup_code = setup_code;
  app_config.homekit.setup_id = setup_id;
  app_config.gate_operator.step.gpio = relay_gpio;
  app_config.gate_operator.step.active_level = parse_level(relay_level);
  app_config.gate_operator.step.pulse_ms = pulse_ms;
  app_config.gate_operator.single_feedback.gpio = sensor_gpio;
  app_config.gate_operator.single_feedback.active_level = parse_level(sensor_level);
  app_config.gate_operator.single_feedback.pull = parse_pull(sensor_pull);
  app_config.gate_operator.single_active_endpoint =
      feedback_endpoint == "open" ? gate::config::FeedbackEndpoint::kOpen
                                  : gate::config::FeedbackEndpoint::kClosed;
  app_config.gate_operator.endpoint_stability_ms = feedback_stability_ms;
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

  gate::bootstrap::set_station(&credentials, ssid, password);
  result = gate::bootstrap::save(credentials);
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

esp_err_t setup_wifi_handler(httpd_req_t* request) {
  if (application_provisioned) {
    return send_error(request, "403 Forbidden", "Configuration is already saved.");
  }
  if (request->content_len <= 0 || request->content_len > 512) {
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
  if (!form_value(body, "ssid", &ssid) || ssid.empty() || ssid.size() > 32) {
    return send_error(request, "400 Bad Request",
                      "Wi-Fi name must contain 1-32 characters.");
  }
  if (!form_value(body, "password", &password) || password.size() > 63 ||
      (!password.empty() && password.size() < 8)) {
    return send_error(request, "400 Bad Request",
                      "Wi-Fi password must be empty or contain 8-63 characters.");
  }

  const gate::bootstrap::Credentials previous = credentials;
  gate::bootstrap::set_station(&credentials, ssid, password);
  std::fill(password.begin(), password.end(), '\0');
  const esp_err_t result = gate::bootstrap::save(credentials);
  if (result != ESP_OK) {
    credentials = previous;
    return send_error(request, "500 Internal Server Error",
                      "Could not save Wi-Fi credentials.");
  }
  httpd_resp_set_type(request, "application/json");
  const esp_err_t response_result =
      httpd_resp_sendstr(request, "{\"saved\":true,\"restarting\":true}");
  if (response_result == ESP_OK) {
    xTaskCreate(restart_task, "wifi_setup_restart", 2048, nullptr, 5, nullptr);
  }
  return response_result;
}

esp_err_t captive_handler(httpd_req_t* request) {
  httpd_resp_set_status(request, "302 Found");
  httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
  httpd_resp_send(request, nullptr, 0);
  return ESP_OK;
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
  const httpd_uri_t setup_wifi{
      .uri = "/api/v1/setup/wifi", .method = HTTP_POST,
      .handler = setup_wifi_handler, .user_ctx = nullptr};
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
      (result = httpd_register_uri_handler(server, &setup_wifi)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &save)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &captive)) != ESP_OK) {
    httpd_stop(server);
    server = nullptr;
  }
  return result;
}

}  // namespace

esp_err_t start() {
  esp_err_t result = gate::bootstrap::load_or_create(&credentials);
  if (result != ESP_OK) return result;

  gate::config::AppConfig saved_config;
  application_provisioned =
      gate::config::ConfigRepository().load(&saved_config) == ESP_OK;
  if (application_provisioned) active_config = std::move(saved_config);

  if (!application_provisioned && credentials.station_ssid[0] != '\0') {
    const esp_err_t connect_result = gate::homekit::connect_bootstrap_station(
        credentials.station_ssid, credentials.station_password);
    if (connect_result != ESP_OK) {
      ESP_LOGW(kTag, "Could not resume staged-onboarding Wi-Fi connection: %s",
               esp_err_to_name(connect_result));
    }
  }

  ESP_RETURN_ON_ERROR(
      gate::network::start({credentials.ap_password,
                            credentials.station_ssid[0] != '\0'}),
      kTag, "Could not start setup network");
  ESP_RETURN_ON_ERROR(start_http_server(), kTag,
                      "Could not start setup web server");
  ESP_RETURN_ON_ERROR(gate::captive_dns::start(), kTag,
                      "Could not start captive DNS");

  ESP_LOGI(kTag, "Setup AP SSID: %s", gate::network::access_point_ssid());
  ESP_LOGI(kTag, "Setup AP password: %s", credentials.ap_password);
  ESP_LOGI(kTag, "Setup URL: http://192.168.4.1/");
  return ESP_OK;
}

}  // namespace gate::provisioning
