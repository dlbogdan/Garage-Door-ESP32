#include "provisioning.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "app_config.hpp"
#include "config_repository.hpp"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "password.hpp"

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

template <std::size_t Size>
void copy_wifi_string(std::uint8_t (&destination)[Size], const char* source) {
  std::memset(destination, 0, Size);
  std::memcpy(destination, source, std::min(std::strlen(source), Size));
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

esp_err_t root_handler(httpd_req_t* request) {
  return send_asset(request, "text/html; charset=utf-8", index_html_start,
                    index_html_end, false);
}

esp_err_t javascript_handler(httpd_req_t* request) {
  return send_asset(request, "text/javascript; charset=utf-8", app_js_start,
                    app_js_end, true);
}

esp_err_t stylesheet_handler(httpd_req_t* request) {
  return send_asset(request, "text/css; charset=utf-8", app_css_start,
                    app_css_end, true);
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
  int relay_gpio = -1;
  int sensor_gpio = -1;
  int pulse_ms = 0;
  int opening_seconds = 0;
  int closing_seconds = 0;
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
      parse_integer(body, "relayGpio", 0, 39, &relay_gpio) &&
      parse_integer(body, "sensorGpio", 0, 39, &sensor_gpio) &&
      parse_integer(body, "pulseMs", 100, 2000, &pulse_ms) &&
      parse_integer(body, "openingSeconds", 3, 180, &opening_seconds) &&
      parse_integer(body, "closingSeconds", 3, 180, &closing_seconds) &&
      admin_password.size() >= 10 && admin_password.size() <= 128 &&
      (relay_level == "low" || relay_level == "high") &&
      (sensor_level == "low" || sensor_level == "high") &&
      (sensor_pull == "none" || sensor_pull == "up" || sensor_pull == "down");
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

void wifi_event(void*, esp_event_base_t event_base, int32_t event_id,
                void*) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START &&
      credentials.station_ssid[0] != '\0') {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED &&
             credentials.station_ssid[0] != '\0') {
    station_connected = false;
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    station_connected = true;
    ESP_LOGI(kTag, "Station connected; setup AP remains active");
  }
}

esp_err_t start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
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
  const httpd_uri_t save{.uri = "/save", .method = HTTP_POST,
                         .handler = save_handler, .user_ctx = nullptr};
  const httpd_uri_t captive{.uri = "/*", .method = HTTP_GET,
                            .handler = captive_handler, .user_ctx = nullptr};
  if ((result = httpd_register_uri_handler(server, &root)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &javascript)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &stylesheet)) != ESP_OK ||
      (result = httpd_register_uri_handler(server, &status)) != ESP_OK ||
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

  std::uint8_t mac[6]{};
  ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), kTag,
                      "Could not read device MAC");
  std::snprintf(access_point_ssid, sizeof(access_point_ssid),
                "GateSetup-%02X%02X%02X", mac[3], mac[4], mac[5]);

  ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "Could not initialize network");
  result = esp_event_loop_create_default();
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
  esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init), kTag,
                      "Could not initialize Wi-Fi");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  wifi_event, nullptr),
                      kTag, "Could not register Wi-Fi events");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  wifi_event, nullptr),
                      kTag, "Could not register IP events");

  wifi_config_t ap{};
  copy_wifi_string(ap.ap.ssid, access_point_ssid);
  copy_wifi_string(ap.ap.password, credentials.ap_password);
  ap.ap.ssid_len = std::strlen(access_point_ssid);
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

  wifi_config_t station{};
  copy_wifi_string(station.sta.ssid, credentials.station_ssid);
  copy_wifi_string(station.sta.password, credentials.station_password);
  station.sta.threshold.authmode = credentials.station_password[0] == '\0'
                                       ? WIFI_AUTH_OPEN
                                       : WIFI_AUTH_WPA2_PSK;

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag,
                      "Could not set AP/station mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), kTag,
                      "Could not configure setup AP");
  if (credentials.station_ssid[0] != '\0') {
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), kTag,
                        "Could not configure station");
  }
  ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "Could not start Wi-Fi");
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
