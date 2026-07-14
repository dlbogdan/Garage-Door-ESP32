#include "network_manager.hpp"

#include <cstdint>
#include <cstdio>

#include "WiFi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

namespace gate::network {
namespace {

constexpr char kTag[] = "network_manager";
char setup_ssid[24]{};
bool connected = false;
bool has_station_credentials = false;
std::uint32_t fallback_delay_ms = 30000;
esp_timer_handle_t fallback_timer = nullptr;
bool access_point_active = false;

void stop_access_point() {
  if (!access_point_active) return;
  WiFi.softAPdisconnect(false);
  access_point_active = false;
  ESP_LOGI(kTag, "Station connected; setup AP stopped");
}

void start_access_point(void*) {
  if (connected || access_point_active) return;
  if (!WiFi.softAP(setup_ssid, nullptr, 1, 0, 4)) {
    ESP_LOGE(kTag, "Could not start open setup AP");
    return;
  }
  access_point_active = true;
  ESP_LOGW(kTag, "Open recovery AP started: %s", setup_ssid);
}

void arm_fallback() {
  if (fallback_timer == nullptr || connected || access_point_active) return;
  esp_timer_stop(fallback_timer);
  esp_timer_start_once(fallback_timer,
                       static_cast<std::uint64_t>(fallback_delay_ms) * 1000);
}

void wifi_event(arduino_event_id_t event, arduino_event_info_t) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED &&
      has_station_credentials) {
    connected = false;
    arm_fallback();
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    connected = true;
    if (fallback_timer != nullptr) esp_timer_stop(fallback_timer);
    stop_access_point();
  }
}

}  // namespace

esp_err_t start(const SetupAccessPoint& config) {
  if (config.password == nullptr) return ESP_ERR_INVALID_ARG;
  has_station_credentials = config.station_credentials_present;
  fallback_delay_ms = config.fallback_delay_ms;

  std::uint8_t mac[6]{};
  esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  if (result != ESP_OK) return result;
  std::snprintf(setup_ssid, sizeof(setup_ssid), "GateSetup-%02X%02X%02X",
                mac[3], mac[4], mac[5]);

  WiFi.onEvent(wifi_event);
  if (!WiFi.mode(has_station_credentials ? WIFI_AP_STA : WIFI_AP)) {
    ESP_LOGE(kTag, "Could not set Arduino Wi-Fi mode");
    return ESP_FAIL;
  }

  const esp_timer_create_args_t timer_args{
      .callback = start_access_point,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_fallback",
      .skip_unhandled_events = true,
  };
  result = esp_timer_create(&timer_args, &fallback_timer);
  if (result != ESP_OK) return result;

  if (!has_station_credentials) {
    start_access_point(nullptr);
  } else {
    arm_fallback();
  }
  if (!has_station_credentials && !access_point_active) {
    return ESP_FAIL;
  }

  // HomeSpan remains the sole owner of station WiFi.begin() and reconnects.
  ESP_LOGI(kTag, "Setup AP SSID: %s (open; active only when needed)", setup_ssid);
  return ESP_OK;
}

bool station_connected() { return connected; }

const char* access_point_ssid() { return setup_ssid; }

}  // namespace gate::network
