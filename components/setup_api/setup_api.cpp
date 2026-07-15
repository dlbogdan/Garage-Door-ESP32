#include "setup_api.hpp"

#include <algorithm>
#include <string>

#include "config_repository.hpp"
#include "esp_log.h"
#include "network_manager.hpp"
#include "password.hpp"

namespace gate::setup_api {
namespace {

constexpr char kTag[] = "setup_api";
constexpr std::size_t kMaxRequestBody = 2048;
Context api_context{};

gate::config::ActiveLevel parse_level(const std::string& value) {
  return value == "high" ? gate::config::ActiveLevel::kHigh
                         : gate::config::ActiveLevel::kLow;
}

gate::config::SensorPull parse_pull(const std::string& value) {
  if (value == "none") return gate::config::SensorPull::kNone;
  if (value == "down") return gate::config::SensorPull::kDown;
  return gate::config::SensorPull::kUp;
}

bool read_body(httpd_req_t* request, std::size_t maximum, std::string* body) {
  if (request->content_len <= 0 || request->content_len > maximum) return false;
  body->assign(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body->size()) {
    const int count = httpd_req_recv(request, body->data() + received,
                                     body->size() - received);
    if (count <= 0) return false;
    received += static_cast<std::size_t>(count);
  }
  return true;
}

esp_err_t status_handler(httpd_req_t* request) {
  const auto& credentials = *api_context.credentials;
  const std::string response =
      "{\"provisioned\":" +
      std::string(*api_context.application_provisioned ? "true" : "false") +
      ",\"connected\":" +
      std::string(gate::network::station_connected() ? "true" : "false") +
      ",\"hasWifi\":" +
      std::string(credentials.station_ssid[0] ? "true" : "false") +
      ",\"ssid\":\"" + api_context.json_escape(credentials.station_ssid) +
      "\",\"apSsid\":\"" +
      api_context.json_escape(gate::network::access_point_ssid()) + "\"}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t save_handler(httpd_req_t* request) {
  if (*api_context.application_provisioned) {
    return api_context.send_error(request, "403 Forbidden",
                                  "Configuration is already saved.");
  }
  std::string body;
  if (!read_body(request, kMaxRequestBody, &body)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Invalid request size.");
  }
  auto& credentials = *api_context.credentials;
  std::string ssid, password, admin_password, display_name, setup_code, setup_id;
  std::string relay_level, sensor_level, sensor_pull, feedback_endpoint;
  int relay_gpio = -1, sensor_gpio = -1, pulse_ms = 0;
  int opening_seconds = 0, closing_seconds = 0, feedback_stability_ms = 0;
  const bool submitted_wifi = api_context.form_value(body, "ssid", &ssid);
  if (submitted_wifi) api_context.form_value(body, "password", &password);
  if (!submitted_wifi && credentials.station_ssid[0] != '\0') {
    ssid = credentials.station_ssid;
    password = credentials.station_password;
  }
  if (ssid.empty() || ssid.size() > 32 || password.size() > 63 ||
      (!password.empty() && password.size() < 8)) {
    return api_context.send_error(
        request, "400 Bad Request",
        "Enter an SSID and either no password or 8-63 characters.");
  }
  if (!api_context.form_value(body, "adminPassword", &admin_password)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Administrator password is missing.");
  }
  if (admin_password.size() < 10 || admin_password.size() > 128) {
    return api_context.send_error(
        request, "400 Bad Request",
        "Administrator password must contain 10-128 characters.");
  }
  if (!api_context.form_value(body, "displayName", &display_name) ||
      display_name.empty()) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Apple Home display name is missing.");
  }
  if (!api_context.form_value(body, "setupCode", &setup_code)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Apple Home setup code is missing.");
  }
  if (!api_context.form_value(body, "setupId", &setup_id)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Apple Home Setup ID is missing.");
  }
  if (!api_context.form_value(body, "relayLevel", &relay_level) ||
      (relay_level != "low" && relay_level != "high")) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Relay active level is invalid.");
  }
  if (!api_context.form_value(body, "sensorLevel", &sensor_level) ||
      (sensor_level != "low" && sensor_level != "high")) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Feedback active level is invalid.");
  }
  if (!api_context.form_value(body, "sensorPull", &sensor_pull) ||
      (sensor_pull != "none" && sensor_pull != "up" && sensor_pull != "down")) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Feedback pull mode is invalid.");
  }
  if (!api_context.form_value(body, "feedbackEndpoint", &feedback_endpoint) ||
      (feedback_endpoint != "open" && feedback_endpoint != "closed")) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Feedback endpoint meaning is invalid.");
  }
  if (!api_context.parse_integer(body, "relayGpio", 0, 39, &relay_gpio)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Relay GPIO must be between 0 and 39.");
  }
  if (!api_context.parse_integer(body, "sensorGpio", 0, 39, &sensor_gpio)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Feedback GPIO must be between 0 and 39.");
  }
  if (!api_context.parse_integer(body, "pulseMs", 100, 2000, &pulse_ms)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Pulse duration must be 100-2000 ms.");
  }
  if (!api_context.parse_integer(body, "openingSeconds", 3, 180,
                                 &opening_seconds)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Opening time must be 3-180 seconds.");
  }
  if (!api_context.parse_integer(body, "closingSeconds", 3, 180,
                                 &closing_seconds)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Closing time must be 3-180 seconds.");
  }
  if (!api_context.parse_integer(body, "feedbackStabilityMs", 1000, 10000,
                                 &feedback_stability_ms)) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Endpoint stability must be 1000-10000 ms.");
  }

  gate::config::AppConfig config;
  config.wifi.ssid = ssid;
  config.wifi.password = password;
  config.access_point.password = credentials.ap_password;
  config.homekit.display_name = display_name;
  config.homekit.setup_code = setup_code;
  config.homekit.setup_id = setup_id;
  config.gate_operator.step = {relay_gpio, parse_level(relay_level),
                               static_cast<std::uint32_t>(pulse_ms)};
  config.gate_operator.single_feedback.gpio = sensor_gpio;
  config.gate_operator.single_feedback.active_level = parse_level(sensor_level);
  config.gate_operator.single_feedback.pull = parse_pull(sensor_pull);
  config.gate_operator.single_active_endpoint =
      feedback_endpoint == "open" ? gate::config::FeedbackEndpoint::kOpen
                                  : gate::config::FeedbackEndpoint::kClosed;
  config.gate_operator.endpoint_stability_ms = feedback_stability_ms;
  config.timing.opening_ms = opening_seconds * 1000;
  config.timing.closing_ms = closing_seconds * 1000;
  esp_err_t result = gate::config::derive_admin_password(admin_password, &config.admin);
  std::fill(admin_password.begin(), admin_password.end(), '\0');
  if (result != ESP_OK) {
    return api_context.send_error(request, "500 Internal Server Error",
                                  "Could not derive administrator credentials.");
  }
  const auto errors = gate::config::validate(config);
  if (!errors.empty()) {
    return api_context.send_error(request, "400 Bad Request",
                                  errors.front().field + ": " + errors.front().message);
  }
  gate::bootstrap::set_station(&credentials, ssid, password);
  result = gate::bootstrap::save(credentials);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Failed to save Wi-Fi credentials: %s", esp_err_to_name(result));
    return api_context.send_error(request, "500 Internal Server Error",
                                  "Could not save settings.");
  }
  result = gate::config::ConfigRepository().save(config);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Failed to save application configuration: %s",
             esp_err_to_name(result));
    return api_context.send_error(request, "500 Internal Server Error",
                                  "Could not save complete configuration.");
  }
  *api_context.application_provisioned = true;
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_sendstr(request,
      "<!doctype html><meta name=viewport content='width=device-width'>"
      "<h1>Saved</h1><p>The controller is restarting and will try your Wi-Fi. "
      "Reconnect to the setup network if needed.</p>");
  api_context.schedule_restart();
  return ESP_OK;
}

esp_err_t setup_wifi_handler(httpd_req_t* request) {
  if (*api_context.application_provisioned) {
    return api_context.send_error(request, "403 Forbidden",
                                  "Configuration is already saved.");
  }
  std::string body;
  if (!read_body(request, 512, &body)) {
    return api_context.send_error(request, "400 Bad Request", "Invalid request size.");
  }
  std::string ssid, password;
  if (!api_context.form_value(body, "ssid", &ssid) || ssid.empty() ||
      ssid.size() > 32) {
    return api_context.send_error(request, "400 Bad Request",
                                  "Wi-Fi name must contain 1-32 characters.");
  }
  if (!api_context.form_value(body, "password", &password) ||
      password.size() > 63 || (!password.empty() && password.size() < 8)) {
    return api_context.send_error(
        request, "400 Bad Request",
        "Wi-Fi password must be empty or contain 8-63 characters.");
  }
  auto& credentials = *api_context.credentials;
  const auto previous = credentials;
  gate::bootstrap::set_station(&credentials, ssid, password);
  std::fill(password.begin(), password.end(), '\0');
  if (gate::bootstrap::save(credentials) != ESP_OK) {
    credentials = previous;
    return api_context.send_error(request, "500 Internal Server Error",
                                  "Could not save Wi-Fi credentials.");
  }
  httpd_resp_set_type(request, "application/json");
  const esp_err_t result =
      httpd_resp_sendstr(request, "{\"saved\":true,\"restarting\":true}");
  if (result == ESP_OK) api_context.schedule_restart();
  return result;
}

}  // namespace

esp_err_t register_routes(httpd_handle_t server, Context context) {
  if (server == nullptr || context.credentials == nullptr ||
      context.application_provisioned == nullptr || context.send_error == nullptr ||
      context.form_value == nullptr || context.parse_integer == nullptr ||
      context.json_escape == nullptr || context.schedule_restart == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  api_context = context;
  const httpd_uri_t status{.uri = "/api/v1/setup/status", .method = HTTP_GET,
                           .handler = status_handler, .user_ctx = nullptr};
  const httpd_uri_t wifi{.uri = "/api/v1/setup/wifi", .method = HTTP_POST,
                         .handler = setup_wifi_handler, .user_ctx = nullptr};
  const httpd_uri_t save{.uri = "/save", .method = HTTP_POST,
                         .handler = save_handler, .user_ctx = nullptr};
  esp_err_t result = httpd_register_uri_handler(server, &status);
  if (result == ESP_OK) result = httpd_register_uri_handler(server, &wifi);
  if (result == ESP_OK) result = httpd_register_uri_handler(server, &save);
  return result;
}

}  // namespace gate::setup_api
