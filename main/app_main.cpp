#include "config_repository.hpp"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "gate_runtime.hpp"
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

  // Initialize Arduino/HomeSpan first so there is exactly one owner for the
  // Wi-Fi driver, default netifs, and Arduino network-event translation.
  gate::homekit::initialize_networking();

  ESP_LOGI(kTag, "Gate Controller bootstrap on ESP-IDF %s", IDF_VER);
  gate::config::AppConfig config;
  gate::config::ConfigRepository repository;
  result = repository.load(&config);
  if (result == ESP_OK) {
    ESP_LOGI(kTag, "Validated configuration schema %lu loaded",
             static_cast<unsigned long>(config.schema_version));
    const esp_err_t runtime_result = gate::runtime::start(config);
    if (runtime_result != ESP_OK) {
      ESP_LOGE(kTag, "Could not start serialized gate runtime: %s",
               esp_err_to_name(runtime_result));
    }
  } else {
    ESP_LOGW(kTag, "No valid provisioned configuration (%s)",
             esp_err_to_name(result));
  }

  result = gate::provisioning::start();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Could not start Wi-Fi setup portal: %s",
             esp_err_to_name(result));
  }

  if (result == ESP_OK && gate::runtime::active()) {
    const esp_err_t homekit_result = gate::homekit::start(config);
    if (homekit_result != ESP_OK) {
      ESP_LOGE(kTag, "Could not start Apple Home service: %s",
               esp_err_to_name(homekit_result));
    }
  }
  ESP_LOGI(kTag, "Bench relay pulse and HomeKit target control enabled");
}
