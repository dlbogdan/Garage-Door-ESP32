#include "config_repository.hpp"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
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
  bool restore_applied = false;
  const esp_err_t restore_result =
      gate::backup::apply_staged_restore_early(&restore_applied);
  if (restore_result != ESP_OK && restore_result != ESP_ERR_NOT_FOUND) {
    ESP_LOGE(kTag, "Staged NVS restore failed; refusing destructive recovery: %s",
             esp_err_to_name(restore_result));
    return;
  }
  if (restore_applied) {
    ESP_LOGI(kTag, "Restarting before initializing clients of the restored NVS");
    esp_restart();
  }

  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGE(kTag,
             "NVS initialization failed with %s; preserving flash instead of erasing all configuration",
             esp_err_to_name(result));
    const esp_err_t retry_result = gate::backup::retry_applied_restore();
    if (retry_result == ESP_OK) {
      ESP_LOGW(kTag, "Restarting to retry the retained restore image");
      esp_restart();
    }
    ESP_LOGE(kTag, "No retained applied restore could be rearmed: %s",
             esp_err_to_name(retry_result));
  }
  if (result != ESP_OK) return;

  // Initialize Arduino/HomeSpan first so there is exactly one owner for the
  // Wi-Fi driver, default netifs, and Arduino network-event translation.
  gate::homekit::initialize_networking();

  ESP_LOGI(kTag, "Gate Controller bootstrap on ESP-IDF %s", IDF_VER);
  gate::config::ConfigRepository repository;
  result = repository.load(&active_config);
  const bool application_provisioned = result == ESP_OK;
  if (!application_provisioned) {
    const esp_err_t retry_result = gate::backup::retry_applied_restore();
    if (retry_result == ESP_OK) {
      ESP_LOGE(kTag,
               "Restored application configuration failed validation (%s); retrying retained image",
               esp_err_to_name(result));
      esp_restart();
    }
  }
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

  esp_err_t homekit_result = ESP_ERR_INVALID_STATE;
  if (result == ESP_OK && gate::runtime::active()) {
    homekit_result = gate::homekit::start(active_config);
    if (homekit_result != ESP_OK) {
      ESP_LOGE(kTag, "Could not start Apple Home service: %s",
               esp_err_to_name(homekit_result));
    }
  }
  gate::ota::schedule_boot_confirmation(
      result == ESP_OK && gate::runtime::active() && gate::homekit::active());
  if (application_provisioned && result == ESP_OK && gate::runtime::active() &&
      homekit_result == ESP_OK && gate::homekit::active()) {
    bool validation_restart_required = false;
    const esp_err_t confirm_result =
        gate::backup::confirm_staged_restore(&validation_restart_required);
    if (confirm_result != ESP_OK) {
      ESP_LOGE(kTag, "Could not erase confirmed restore staging payload: %s",
               esp_err_to_name(confirm_result));
    }
    if (validation_restart_required) {
      ESP_LOGI(kTag, "Restarting for final restored-NVS validation boot");
      esp_restart();
    }
  }
  ESP_LOGI(kTag, "Bench relay pulse and HomeKit target control enabled");
}
