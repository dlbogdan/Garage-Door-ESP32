#include "config_repository.hpp"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "gate_runtime.hpp"
#include "homespan_compatibility.hpp"
#include "nvs_flash.h"
#include "ota_manager.hpp"
#include "provisioning.hpp"
#include "restore_staging.hpp"

namespace {
constexpr char kTag[] = "gate_app";

// Schema v4 contains bounded decoder tables and is intentionally much larger
// than the legacy endpoint-only model. Keep the long-lived active configuration
// in static storage instead of consuming most of app_main's 3584-byte stack.
gate::config::AppConfig active_config;

static_assert(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5, 5, 4),
              "This firmware supports ESP-IDF v5.5.4 only");
}  // namespace

extern "C" void app_main(void) {
  const esp_err_t restore_result = gate::backup::apply_staged_restore_early();
  if (restore_result != ESP_OK && restore_result != ESP_ERR_NOT_FOUND) {
    ESP_LOGE(kTag, "Staged NVS restore failed safely: %s",
             esp_err_to_name(restore_result));
  }

  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(result);

  // Initialize Arduino/HomeSpan first so there is exactly one owner for the
  // Wi-Fi driver, default netifs, and Arduino network-event translation.
  gate::homekit::initialize_networking();

  ESP_LOGI(kTag, "Gate Controller bootstrap on ESP-IDF %s", IDF_VER);
  gate::config::ConfigRepository repository;
  result = repository.load(&active_config);
  const bool application_provisioned = result == ESP_OK;
  if (application_provisioned) {
    ESP_LOGI(kTag, "Validated configuration schema %lu loaded",
             static_cast<unsigned long>(active_config.schema_version));
    const esp_err_t runtime_result = gate::runtime::start(active_config);
    if (runtime_result != ESP_OK) {
      ESP_LOGE(kTag, "Could not start serialized gate runtime: %s",
               esp_err_to_name(runtime_result));
    }
  } else {
    ESP_LOGW(kTag, "No valid provisioned configuration (%s)",
             esp_err_to_name(result));
  }

  result = gate::provisioning::start(application_provisioned ? &active_config
                                                             : nullptr);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Could not start Wi-Fi setup portal: %s",
             esp_err_to_name(result));
  }

  if (result == ESP_OK && gate::runtime::active()) {
    const esp_err_t homekit_result = gate::homekit::start(active_config);
    if (homekit_result != ESP_OK) {
      ESP_LOGE(kTag, "Could not start Apple Home service: %s",
               esp_err_to_name(homekit_result));
    }
  }
  gate::ota::schedule_boot_confirmation(
      result == ESP_OK && gate::runtime::active() && gate::homekit::active());
  ESP_LOGI(kTag, "Bench relay pulse and HomeKit target control enabled");
}
