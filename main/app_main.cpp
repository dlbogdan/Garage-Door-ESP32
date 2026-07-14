#include "config_repository.hpp"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "gate_controller.hpp"
#include "gate_hardware.hpp"
#include "homespan_compatibility.hpp"
#include "nvs_flash.h"
#include "provisioning.hpp"

namespace {
constexpr char kTag[] = "gate_app";

static_assert(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5, 5, 4),
              "This firmware supports ESP-IDF v5.5.4 only");
}  // namespace

extern "C" void app_main(void) {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(result);

  ESP_LOGI(kTag, "Gate Controller bootstrap on ESP-IDF %s", IDF_VER);
  gate::config::AppConfig config;
  gate::config::ConfigRepository repository;
  result = repository.load(&config);
  if (result == ESP_OK) {
    ESP_LOGI(kTag, "Validated configuration schema %lu loaded",
             static_cast<unsigned long>(config.schema_version));
    const esp_err_t hardware_result = gate::hardware::start_monitoring(config);
    if (hardware_result != ESP_OK) {
      ESP_LOGE(kTag, "Could not start safe hardware monitoring: %s",
               esp_err_to_name(hardware_result));
    }
  } else {
    ESP_LOGW(kTag, "No valid provisioned configuration (%s)",
             esp_err_to_name(result));
  }

  const gate::controller::Snapshot initial_snapshot{};
  const auto boot_transition = gate::controller::reduce(
      initial_snapshot,
      {gate::controller::EventType::kBoot,
       gate::controller::Target::kOpen,
       false});
  ESP_LOGI(kTag, "Gate controller initialized safely in state %s",
           gate::controller::to_string(boot_transition.next.state));
  ESP_ERROR_CHECK_WITHOUT_ABORT(
      boot_transition.effects.start_pulse ? ESP_ERR_INVALID_STATE : ESP_OK);

  result = gate::provisioning::start();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Could not start Wi-Fi setup portal: %s",
             esp_err_to_name(result));
  }

  ESP_LOGI(kTag, "HomeSpan Garage Door service compatibility compiled in");
  ESP_LOGW(kTag, "Relay pulse control remains disabled in this milestone");
}
