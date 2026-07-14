#include "network_manager.hpp"

#include <cstdint>
#include <cstdio>

#include "WiFi.h"
#include "esp_log.h"
#include "esp_mac.h"

namespace gate::network {
namespace {

constexpr char kTag[] = "network_manager";
char setup_ssid[24]{};
bool connected = false;
bool has_station_credentials = false;

void wifi_event(arduino_event_id_t event, arduino_event_info_t) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED &&
      has_station_credentials) {
    connected = false;
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    connected = true;
    ESP_LOGI(kTag, "Station connected; setup AP remains active");
  }
}

}  // namespace

esp_err_t start(const SetupAccessPoint& config) {
  if (config.password == nullptr) return ESP_ERR_INVALID_ARG;
  has_station_credentials = config.station_credentials_present;

  std::uint8_t mac[6]{};
  esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  if (result != ESP_OK) return result;
  std::snprintf(setup_ssid, sizeof(setup_ssid), "GateSetup-%02X%02X%02X",
                mac[3], mac[4], mac[5]);

  WiFi.onEvent(wifi_event);
  if (!WiFi.mode(WIFI_AP_STA)) {
    ESP_LOGE(kTag, "Could not set Arduino Wi-Fi AP+STA mode");
    return ESP_FAIL;
  }
  if (!WiFi.softAP(setup_ssid, config.password, 1, 0, 4)) {
    ESP_LOGE(kTag, "Could not configure Arduino setup AP");
    return ESP_FAIL;
  }

  // HomeSpan remains the sole owner of station WiFi.begin() and reconnects.
  ESP_LOGI(kTag, "Setup AP SSID: %s", setup_ssid);
  return ESP_OK;
}

bool station_connected() { return connected; }

const char* access_point_ssid() { return setup_ssid; }

}  // namespace gate::network
