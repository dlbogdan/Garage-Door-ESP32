#include "gate_hardware.hpp"

#include <atomic>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_debouncer.hpp"

namespace gate::hardware {
namespace {

constexpr char kTag[] = "gate_hardware";
constexpr TickType_t kSamplePeriod = pdMS_TO_TICKS(10);
gate::config::SensorConfig sensor_config;
std::atomic_bool monitor_active{false};
std::atomic_bool stable_sensor_active{false};

bool sensor_active() {
  const bool high = gpio_get_level(static_cast<gpio_num_t>(sensor_config.gpio)) != 0;
  return sensor_config.active_level == gate::config::ActiveLevel::kHigh ? high
                                                                        : !high;
}

std::uint32_t now_ms() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

void sensor_task(void*) {
  const bool initial = sensor_active();
  stable_sensor_active.store(initial);
  SensorDebouncer debouncer(initial, sensor_config.debounce_ms, now_ms());
  ESP_LOGI(kTag, "Sensor GPIO %d initial raw=%d, active level=%s, state=%s",
           sensor_config.gpio,
           gpio_get_level(static_cast<gpio_num_t>(sensor_config.gpio)),
           sensor_config.active_level == gate::config::ActiveLevel::kHigh
               ? "high"
               : "low",
           initial ? "closed" : "not closed");
  while (true) {
    vTaskDelay(kSamplePeriod);
    const SensorUpdate update = debouncer.update(sensor_active(), now_ms());
    if (update.changed) {
      stable_sensor_active.store(update.active);
      ESP_LOGI(kTag, "Sensor GPIO %d stable raw=%d, state=%s",
               sensor_config.gpio,
               gpio_get_level(static_cast<gpio_num_t>(sensor_config.gpio)),
               update.active ? "closed" : "not closed");
    }
  }
}

}  // namespace

esp_err_t start_monitoring(const gate::config::AppConfig& config) {
  const std::uint32_t inactive =
      config.relay.active_level == gate::config::ActiveLevel::kHigh ? 0 : 1;
  const gpio_num_t relay_gpio = static_cast<gpio_num_t>(config.relay.gpio);
  const gpio_num_t sensor_gpio = static_cast<gpio_num_t>(config.sensor.gpio);
  // Set the output latch before enabling output mode to avoid an active glitch.
  esp_err_t result = gpio_set_level(relay_gpio, inactive);
  if (result != ESP_OK) return result;
  result = gpio_set_direction(relay_gpio, GPIO_MODE_OUTPUT);
  if (result != ESP_OK) return result;
  result = gpio_set_level(relay_gpio, inactive);
  if (result != ESP_OK) return result;

  sensor_config = config.sensor;
  result = gpio_set_direction(sensor_gpio, GPIO_MODE_INPUT);
  if (result != ESP_OK) return result;
  switch (sensor_config.pull) {
    case gate::config::SensorPull::kUp:
      result = gpio_set_pull_mode(sensor_gpio, GPIO_PULLUP_ONLY);
      break;
    case gate::config::SensorPull::kDown:
      result = gpio_set_pull_mode(sensor_gpio, GPIO_PULLDOWN_ONLY);
      break;
    case gate::config::SensorPull::kNone:
      result = gpio_set_pull_mode(sensor_gpio, GPIO_FLOATING);
      break;
  }
  if (result != ESP_OK) return result;

  if (xTaskCreate(sensor_task, "closed_sensor", 3072, nullptr, 5, nullptr) !=
      pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  monitor_active.store(true);
  ESP_LOGI(kTag,
           "Relay GPIO %d initialized inactive at level %lu; pulse control disabled",
           config.relay.gpio, static_cast<unsigned long>(inactive));
  return ESP_OK;
}

bool monitoring_active() { return monitor_active.load(); }

bool closed_sensor_active() { return stable_sensor_active.load(); }

}  // namespace gate::hardware
