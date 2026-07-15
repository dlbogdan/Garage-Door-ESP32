#include "ota_manager.hpp"

#include <atomic>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gate_runtime.hpp"

// Arduino-ESP32 defines this weakly and otherwise validates a pending image
// before app_main runs. Keep confirmation under application health policy.
extern "C" bool verifyRollbackLater() { return true; }

namespace gate::ota {
namespace {

constexpr char kTag[] = "ota_manager";
constexpr char kExpectedProject[] = "garage_door_esp32";
constexpr TickType_t kConfirmationDelay = pdMS_TO_TICKS(15000);

struct State {
  esp_ota_handle_t handle{0};
  const esp_partition_t* partition{nullptr};
  std::size_t total{0};
  std::size_t written{0};
  Phase phase{Phase::kIdle};
  char error[96]{};
};

State state;
std::atomic_bool locked{false};
portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;

void set_error(const char* reason) {
  portENTER_CRITICAL(&state_lock);
  state.phase = Phase::kFailed;
  std::strncpy(state.error, reason == nullptr ? "OTA failed" : reason,
               sizeof(state.error) - 1);
  state.error[sizeof(state.error) - 1] = '\0';
  portEXIT_CRITICAL(&state_lock);
}

void release_maintenance() {
  gate::runtime::leave_maintenance();
  locked.store(false);
}

void confirmation_task(void*) {
  vTaskDelay(kConfirmationDelay);
  const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
  if (result == ESP_OK) {
    ESP_LOGI(kTag, "Pending firmware passed local health soak and is VALID");
  } else {
    ESP_LOGE(kTag, "Could not confirm pending firmware: %s",
             esp_err_to_name(result));
  }
  vTaskDelete(nullptr);
}

}  // namespace

esp_err_t begin(std::size_t content_length) {
  bool expected = false;
  if (!locked.compare_exchange_strong(expected, true)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (content_length == 0) {
    locked.store(false);
    return ESP_ERR_INVALID_SIZE;
  }
  if (!gate::runtime::enter_maintenance()) {
    locked.store(false);
    return ESP_ERR_INVALID_STATE;
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
  if (update == nullptr || update == running || content_length > update->size) {
    release_maintenance();
    return content_length > (update == nullptr ? 0 : update->size)
               ? ESP_ERR_INVALID_SIZE
               : ESP_ERR_NOT_FOUND;
  }

  // Passing content_length makes esp_ota_begin erase the complete image range
  // synchronously. A ~1.5 MiB erase can starve IDLE0 long enough to trip the
  // task watchdog while flash-cache coordination runs on the other core.
  // Uploads are strictly sequential, so let esp_ota_write erase incrementally.
  esp_ota_handle_t handle = 0;
  ESP_LOGI(kTag, "Preparing sequential %u-byte update in %s",
           static_cast<unsigned>(content_length), update->label);
  const esp_err_t result =
      esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
  if (result != ESP_OK) {
    release_maintenance();
    return result;
  }

  portENTER_CRITICAL(&state_lock);
  state = {};
  state.handle = handle;
  state.partition = update;
  state.total = content_length;
  state.phase = Phase::kReceiving;
  portEXIT_CRITICAL(&state_lock);
  ESP_LOGI(kTag, "Receiving %u-byte update into %s",
           static_cast<unsigned>(content_length), update->label);
  return ESP_OK;
}

esp_err_t write(const void* data, std::size_t length) {
  if (!locked.load() || data == nullptr || length == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (state.phase != Phase::kReceiving ||
      length > state.total - state.written) {
    return ESP_ERR_INVALID_SIZE;
  }
  const esp_err_t result = esp_ota_write(state.handle, data, length);
  if (result != ESP_OK) {
    abort("Flash write failed");
    return result;
  }
  portENTER_CRITICAL(&state_lock);
  state.written += length;
  portEXIT_CRITICAL(&state_lock);
  return ESP_OK;
}

esp_err_t finish() {
  if (!locked.load() || state.phase != Phase::kReceiving) {
    return ESP_ERR_INVALID_STATE;
  }
  if (state.written != state.total) {
    abort("Upload was truncated");
    return ESP_ERR_INVALID_SIZE;
  }

  portENTER_CRITICAL(&state_lock);
  state.phase = Phase::kVerifying;
  portEXIT_CRITICAL(&state_lock);
  esp_err_t result = esp_ota_end(state.handle);
  state.handle = 0;
  if (result != ESP_OK) {
    set_error("Image validation failed");
    release_maintenance();
    return result;
  }

  esp_app_desc_t descriptor{};
  result = esp_ota_get_partition_description(state.partition, &descriptor);
  if (result != ESP_OK ||
      std::strncmp(descriptor.project_name, kExpectedProject,
                   sizeof(descriptor.project_name)) != 0) {
    set_error("Image is not Garage-Door-ESP32 firmware");
    release_maintenance();
    return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
  }
  if (std::strncmp(descriptor.version, esp_app_get_description()->version,
                   sizeof(descriptor.version)) == 0) {
    set_error("Firmware version is already installed");
    release_maintenance();
    return ESP_ERR_INVALID_VERSION;
  }

  result = esp_ota_set_boot_partition(state.partition);
  if (result != ESP_OK) {
    set_error("Could not select new boot partition");
    release_maintenance();
    return result;
  }
  portENTER_CRITICAL(&state_lock);
  state.phase = Phase::kReadyToReboot;
  portEXIT_CRITICAL(&state_lock);
  ESP_LOGI(kTag, "Firmware %s verified; reboot required", descriptor.version);
  return ESP_OK;
}

void abort(const char* reason) {
  if (!locked.load()) return;
  if (state.handle != 0) {
    esp_ota_abort(state.handle);
    state.handle = 0;
  }
  set_error(reason);
  release_maintenance();
  ESP_LOGW(kTag, "OTA aborted: %s", reason == nullptr ? "unknown" : reason);
}

Status status() {
  const esp_app_desc_t* app = esp_app_get_description();
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
  const bool pending = running != nullptr &&
                       esp_ota_get_state_partition(running, &image_state) == ESP_OK &&
                       image_state == ESP_OTA_IMG_PENDING_VERIFY;
  portENTER_CRITICAL(&state_lock);
  const Status output{state.phase,
                      app->version,
                      app->project_name,
                      running == nullptr ? "" : running->label,
                      update == nullptr ? "" : update->label,
                      update == nullptr ? 0 : update->size,
                      state.total,
                      state.written,
                      pending,
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
                      true,
#else
                      false,
#endif
                      state.error};
  portEXIT_CRITICAL(&state_lock);
  return output;
}

bool in_progress() {
  return locked.load() && state.phase != Phase::kReadyToReboot;
}

void schedule_boot_confirmation(bool local_health_ok) {
  esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr ||
      esp_ota_get_state_partition(running, &image_state) != ESP_OK ||
      image_state != ESP_OTA_IMG_PENDING_VERIFY) {
    return;
  }
  if (!local_health_ok) {
    ESP_LOGE(kTag, "Pending firmware failed local startup health checks");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return;
  }
  if (xTaskCreate(confirmation_task, "ota_confirm", 3072, nullptr, 4, nullptr) !=
      pdPASS) {
    ESP_LOGE(kTag, "Could not create OTA confirmation task; rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

const char* phase_name(Phase phase) {
  switch (phase) {
    case Phase::kIdle: return "idle";
    case Phase::kReceiving: return "receiving";
    case Phase::kVerifying: return "verifying";
    case Phase::kReadyToReboot: return "ready_to_reboot";
    case Phase::kFailed: return "failed";
  }
  return "unknown";
}

}  // namespace gate::ota
