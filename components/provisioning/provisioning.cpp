#include "provisioning.hpp"

#include <utility>

#include "app_config.hpp"
#include "bootstrap_credentials.hpp"
#include "captive_dns.hpp"
#include "config_repository.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "homespan_compatibility.hpp"
#include "management_server.hpp"
#include "network_manager.hpp"

namespace gate::provisioning {
namespace {

constexpr char kTag[] = "provisioning";

}  // namespace

esp_err_t start() {
  gate::bootstrap::Credentials credentials;
  esp_err_t result = gate::bootstrap::load_or_create(&credentials);
  if (result != ESP_OK) return result;

  gate::config::AppConfig active_config;
  const bool application_provisioned =
      gate::config::ConfigRepository().load(&active_config) == ESP_OK;

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
                            application_provisioned,
                            application_provisioned
                                ? active_config.wifi.connection_deadline_ms
                                : 30000}),
      kTag, "Could not start setup network");
  ESP_RETURN_ON_ERROR(
      gate::management::start(credentials, application_provisioned,
                              application_provisioned ? &active_config : nullptr),
      kTag, "Could not start management web server");
  ESP_RETURN_ON_ERROR(gate::captive_dns::start(), kTag,
                      "Could not start captive DNS");

  ESP_LOGI(kTag, "Setup AP SSID: %s", gate::network::access_point_ssid());
  ESP_LOGI(kTag,
           "Setup AP security: open (management requires administrator login "
           "after provisioning)");
  ESP_LOGI(kTag, "Setup URL: http://192.168.4.1/");
  return ESP_OK;
}

}  // namespace gate::provisioning
