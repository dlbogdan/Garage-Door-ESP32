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

gate::config::OperatorConfig operator_config;
std::atomic_bool monitor_active{false};
std::atomic_bool opened_asserted{false};
std::atomic_bool closed_asserted{false};
std::atomic<gate::controller::ActuatorCommand> current_command{
    gate::controller::ActuatorCommand::kNone};
FeedbackChangedCallback feedback_callback{nullptr};
void* feedback_context{nullptr};

const gate::config::PulseOutputConfig* output_for(
    gate::controller::ActuatorCommand command) {
  using gate::controller::ActuatorCommand;
  switch (command) {
    case ActuatorCommand::kStep:
      return operator_config.profile == gate::config::OperatorProfile::kSequential
                 ? &operator_config.step
                 : nullptr;
    case ActuatorCommand::kOpen:
      return operator_config.profile == gate::config::OperatorProfile::kDirectional
                 ? &operator_config.open
                 : nullptr;
    case ActuatorCommand::kClose:
      return operator_config.profile == gate::config::OperatorProfile::kDirectional
                 ? &operator_config.close
                 : nullptr;
    case ActuatorCommand::kNone: return nullptr;
  }
  return nullptr;
}

std::uint32_t output_level(const gate::config::PulseOutputConfig& output,
                           bool active) {
  const bool active_high =
      output.active_level == gate::config::ActiveLevel::kHigh;
  return active == active_high ? 1U : 0U;
}

bool input_asserted(const gate::config::FeedbackInputConfig& input) {
  const bool high =
      gpio_get_level(static_cast<gpio_num_t>(input.gpio)) != 0;
  return input.active_level == gate::config::ActiveLevel::kHigh ? high : !high;
}

esp_err_t set_output(const gate::config::PulseOutputConfig& output,
                     bool active) {
  return gpio_set_level(static_cast<gpio_num_t>(output.gpio),
                        output_level(output, active));
}

esp_err_t initialize_output(const gate::config::PulseOutputConfig& output) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(output.gpio);
  esp_err_t result = set_output(output, false);
  if (result == ESP_OK) result = gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
  if (result == ESP_OK) result = set_output(output, false);
  return result;
}

esp_err_t initialize_input(const gate::config::FeedbackInputConfig& input) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(input.gpio);
  esp_err_t result = gpio_set_direction(gpio, GPIO_MODE_INPUT);
  if (result != ESP_OK) return result;
  switch (input.pull) {
    case gate::config::SensorPull::kUp:
      return gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    case gate::config::SensorPull::kDown:
      return gpio_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY);
    case gate::config::SensorPull::kNone:
      return gpio_set_pull_mode(gpio, GPIO_FLOATING);
  }
  return ESP_ERR_INVALID_ARG;
}

std::uint32_t now_ms() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

void feedback_task(void*) {
  const bool dual = operator_config.feedback_topology ==
                    gate::config::FeedbackTopology::kDual;
  const auto& first = dual ? operator_config.opened_feedback
                           : operator_config.single_feedback;
  const auto& second = operator_config.closed_feedback;
  SensorDebouncer first_debouncer(input_asserted(first), first.debounce_ms,
                                   now_ms());
  SensorDebouncer second_debouncer(
      dual ? input_asserted(second) : false,
      dual ? second.debounce_ms : first.debounce_ms, now_ms());

  bool first_stable = input_asserted(first);
  bool second_stable = dual ? input_asserted(second) : false;
  if (dual) {
    opened_asserted.store(first_stable);
    closed_asserted.store(second_stable);
  } else if (operator_config.single_active_endpoint ==
             gate::config::FeedbackEndpoint::kOpen) {
    opened_asserted.store(first_stable);
  } else {
    closed_asserted.store(first_stable);
  }

  while (true) {
    vTaskDelay(kSamplePeriod);
    bool changed = false;
    const SensorUpdate first_update =
        first_debouncer.update(input_asserted(first), now_ms());
    if (first_update.changed) {
      first_stable = first_update.active;
      changed = true;
    }
    if (dual) {
      const SensorUpdate second_update =
          second_debouncer.update(input_asserted(second), now_ms());
      if (second_update.changed) {
        second_stable = second_update.active;
        changed = true;
      }
      opened_asserted.store(first_stable);
      closed_asserted.store(second_stable);
    } else if (operator_config.single_active_endpoint ==
               gate::config::FeedbackEndpoint::kOpen) {
      opened_asserted.store(first_stable);
      closed_asserted.store(false);
    } else {
      opened_asserted.store(false);
      closed_asserted.store(first_stable);
    }
    if (changed && feedback_callback != nullptr) {
      feedback_callback(feedback_assertions(), feedback_context);
    }
  }
}

}  // namespace

esp_err_t start_monitoring(const gate::config::AppConfig& config,
                           FeedbackChangedCallback callback,
                           void* callback_context) {
  if (monitor_active.load()) return ESP_ERR_INVALID_STATE;
  operator_config = config.gate_operator;
  feedback_callback = callback;
  feedback_context = callback_context;

  const gate::config::PulseOutputConfig* outputs[2]{};
  std::size_t output_count = 0;
  if (operator_config.profile == gate::config::OperatorProfile::kSequential) {
    outputs[output_count++] = &operator_config.step;
  } else {
    outputs[output_count++] = &operator_config.open;
    outputs[output_count++] = &operator_config.close;
  }
  for (std::size_t index = 0; index < output_count; ++index) {
    const esp_err_t result = initialize_output(*outputs[index]);
    if (result != ESP_OK) {
      deactivate_all();
      return result;
    }
  }

  const bool dual = operator_config.feedback_topology ==
                    gate::config::FeedbackTopology::kDual;
  esp_err_t result = initialize_input(
      dual ? operator_config.opened_feedback : operator_config.single_feedback);
  if (result == ESP_OK && dual) {
    result = initialize_input(operator_config.closed_feedback);
  }
  if (result != ESP_OK) {
    deactivate_all();
    return result;
  }

  if (xTaskCreate(feedback_task, "gate_feedback", 3072, nullptr, 5, nullptr) !=
      pdPASS) {
    deactivate_all();
    return ESP_ERR_NO_MEM;
  }
  monitor_active.store(true);
  ESP_LOGI(kTag, "Semantic gate I/O initialized with %lu output(s)",
           static_cast<unsigned long>(output_count));
  return ESP_OK;
}

bool monitoring_active() { return monitor_active.load(); }

gate::controller::FeedbackAssertions feedback_assertions() {
  return {opened_asserted.load(), closed_asserted.load()};
}

esp_err_t activate_command(gate::controller::ActuatorCommand command) {
  if (!monitor_active.load() || command == gate::controller::ActuatorCommand::kNone) {
    return ESP_ERR_INVALID_STATE;
  }
  gate::controller::ActuatorCommand expected =
      gate::controller::ActuatorCommand::kNone;
  if (!current_command.compare_exchange_strong(expected, command)) {
    return ESP_ERR_INVALID_STATE;
  }
  const auto* selected = output_for(command);
  if (selected == nullptr) {
    current_command.store(gate::controller::ActuatorCommand::kNone);
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t result = deactivate_all();
  current_command.store(command);
  if (result == ESP_OK) result = set_output(*selected, true);
  if (result != ESP_OK) deactivate_all();
  return result;
}

esp_err_t deactivate_all() {
  current_command.store(gate::controller::ActuatorCommand::kNone);
  esp_err_t first_error = ESP_OK;
  const auto deactivate = [&first_error](
                              const gate::config::PulseOutputConfig& output) {
    const esp_err_t result = set_output(output, false);
    if (first_error == ESP_OK && result != ESP_OK) first_error = result;
  };
  if (operator_config.profile == gate::config::OperatorProfile::kSequential) {
    if (operator_config.step.gpio >= 0) deactivate(operator_config.step);
  } else {
    if (operator_config.open.gpio >= 0) deactivate(operator_config.open);
    if (operator_config.close.gpio >= 0) deactivate(operator_config.close);
  }
  return first_error;
}

gate::controller::ActuatorCommand active_command() {
  return current_command.load();
}

}  // namespace gate::hardware
