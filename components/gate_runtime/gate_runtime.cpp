#include "gate_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <new>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gate_controller.hpp"
#include "gate_hardware.hpp"
#include "relay_pulse_guard.hpp"

namespace gate::runtime {
namespace {

constexpr char kTag[] = "gate_runtime";
constexpr UBaseType_t kQueueLength = 16;
constexpr std::uint32_t kNoDeadline = std::numeric_limits<std::uint32_t>::max();

struct RuntimeMessage {
  gate::controller::Event event;
  TaskHandle_t reply_task{nullptr};
};

struct Deadline {
  bool active{false};
  std::uint32_t at_ms{0};
};

struct RuntimeState {
  gate::config::AppConfig config;
  QueueHandle_t queue{nullptr};
  gate::controller::Snapshot snapshot;
  gate::controller::RelayPulseGuard* pulse_guard{nullptr};
  Deadline pulse;
  Deadline opening;
  Deadline closing;
  Deadline feedback_stability;
};

RuntimeState runtime;
std::atomic_bool runtime_active{false};
portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

void release_runtime_resources() {
  delete runtime.pulse_guard;
  runtime.pulse_guard = nullptr;
  if (runtime.queue != nullptr) {
    vQueueDelete(runtime.queue);
    runtime.queue = nullptr;
  }
}

std::uint32_t now_ms() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

bool reached(std::uint32_t now, std::uint32_t deadline) {
  return static_cast<std::int32_t>(now - deadline) >= 0;
}

void arm(Deadline* deadline, std::uint32_t now, std::uint32_t duration_ms) {
  deadline->active = true;
  deadline->at_ms = now + duration_ms;
}

gate::controller::OperatorProfile controller_profile() {
  return runtime.config.gate_operator.profile ==
                 gate::config::OperatorProfile::kDirectional
             ? gate::controller::OperatorProfile::kDirectional
             : gate::controller::OperatorProfile::kSequential;
}

gate::controller::FeedbackTopology controller_feedback_topology() {
  return runtime.config.gate_operator.feedback_topology ==
                 gate::config::FeedbackTopology::kDual
             ? gate::controller::FeedbackTopology::kDual
             : gate::controller::FeedbackTopology::kSingle;
}

gate::controller::Endpoint single_endpoint() {
  return runtime.config.gate_operator.single_active_endpoint ==
                 gate::config::FeedbackEndpoint::kOpen
             ? gate::controller::Endpoint::kOpened
             : gate::controller::Endpoint::kClosed;
}

gate::controller::OperatorCapabilities capabilities() {
  const bool directional = controller_profile() ==
                           gate::controller::OperatorProfile::kDirectional;
  return {!directional, directional, directional,
          controller_feedback_topology() ==
              gate::controller::FeedbackTopology::kDual};
}

gate::controller::EndpointObservation normalize(
    gate::controller::FeedbackAssertions assertions) {
  return gate::controller::normalize_feedback(controller_feedback_topology(),
                                               single_endpoint(), assertions);
}

void sensor_changed(gate::controller::FeedbackAssertions assertions,
                    void* context) {
  auto* queue = static_cast<QueueHandle_t*>(context);
  const RuntimeMessage message{{
      gate::controller::EventType::kObservationChanged,
      gate::controller::Target::kOpen,
      normalize(assertions)}};
  if (xQueueSend(*queue, &message, portMAX_DELAY) != pdPASS) {
    ESP_LOGE(kTag, "Could not enqueue closed-sensor transition");
  }
}

RequestResult apply_event(const gate::controller::Event& event,
                          std::uint32_t now) {
  const auto transition = gate::controller::reduce(
      runtime.snapshot, event, {controller_profile(), capabilities()});

  // Pulse admission happens before snapshot commit. A rejected or failed pulse
  // therefore cannot falsely advance target/movement state.
  if (transition.effects.actuator_command !=
      gate::controller::ActuatorCommand::kNone) {
    if (!runtime.pulse_guard->can_start(now)) {
      ESP_LOGW(kTag, "Pulse rejected by overlap/minimum-interval guard");
      return RequestResult::kBusy;
    }
    const esp_err_t result = gate::hardware::activate_command(
        transition.effects.actuator_command);
    if (result != ESP_OK) {
      ESP_LOGE(kTag, "Pulse activation failed: %s", esp_err_to_name(result));
      gate::hardware::deactivate_all();
      return RequestResult::kHardwareError;
    }
    runtime.pulse_guard->mark_started(now);
    const auto& operator_config = runtime.config.gate_operator;
    const auto& output =
        transition.effects.actuator_command == gate::controller::ActuatorCommand::kStep
            ? operator_config.step
            : transition.effects.actuator_command == gate::controller::ActuatorCommand::kOpen
                  ? operator_config.open
                  : operator_config.close;
    arm(&runtime.pulse, now, output.pulse_ms);
    ESP_LOGW(kTag, "%s pulse started for %lu ms",
             gate::controller::to_string(transition.effects.actuator_command),
             static_cast<unsigned long>(output.pulse_ms));
  }

  taskENTER_CRITICAL(&snapshot_lock);
  runtime.snapshot = transition.next;
  taskEXIT_CRITICAL(&snapshot_lock);
  if (transition.effects.cancel_travel_timers) {
    runtime.opening.active = false;
    runtime.closing.active = false;
    runtime.feedback_stability.active = false;
  }
  if (transition.effects.start_opening_timer) {
    runtime.closing.active = false;
    arm(&runtime.opening, now, runtime.config.timing.opening_ms);
  }
  if (transition.effects.start_closing_timer) {
    runtime.opening.active = false;
    arm(&runtime.closing, now, runtime.config.timing.closing_ms);
  }
  if (transition.effects.start_feedback_stability_timer) {
    arm(&runtime.feedback_stability, now,
        runtime.config.gate_operator.endpoint_stability_ms);
  }
  ESP_LOGI(kTag, "Controller state=%s target=%s observation=%s valid=%d pulse=%d fault=%s",
           gate::controller::to_string(runtime.snapshot.state),
           gate::controller::to_string(runtime.snapshot.target),
           gate::controller::to_string(runtime.snapshot.pending_observation),
           runtime.snapshot.observation_valid, runtime.snapshot.pulse_active,
           gate::controller::to_string(runtime.snapshot.fault));
  if (transition.command_result == gate::controller::CommandResult::kRejectedBusy) {
    return RequestResult::kBusy;
  }
  return RequestResult::kAccepted;
}

void process_expired(std::uint32_t now) {
  if (runtime.pulse.active && reached(now, runtime.pulse.at_ms)) {
    runtime.pulse.active = false;
    const esp_err_t result = gate::hardware::deactivate_all();
    runtime.pulse_guard->mark_completed();
    if (result != ESP_OK) {
      ESP_LOGE(kTag, "Fail-safe relay deactivation failed: %s",
               esp_err_to_name(result));
    }
    apply_event({gate::controller::EventType::kPulseCompleted}, now);
  }
  if (runtime.opening.active && reached(now, runtime.opening.at_ms)) {
    runtime.opening.active = false;
    apply_event({gate::controller::EventType::kOpeningTimerExpired}, now);
  }
  if (runtime.closing.active && reached(now, runtime.closing.at_ms)) {
    runtime.closing.active = false;
    apply_event({gate::controller::EventType::kClosingTimerExpired}, now);
  }
  if (runtime.feedback_stability.active &&
      reached(now, runtime.feedback_stability.at_ms)) {
    runtime.feedback_stability.active = false;
    apply_event({gate::controller::EventType::kObservationStable,
                 gate::controller::Target::kOpen,
                 runtime.snapshot.pending_observation}, now);
  }
}

TickType_t wait_ticks(std::uint32_t now) {
  std::uint32_t wait_ms = kNoDeadline;
  const Deadline* deadlines[] = {&runtime.pulse, &runtime.opening,
                                  &runtime.closing, &runtime.feedback_stability};
  for (const Deadline* deadline : deadlines) {
    if (!deadline->active) continue;
    if (reached(now, deadline->at_ms)) return 0;
    wait_ms = std::min(wait_ms, deadline->at_ms - now);
  }
  return wait_ms == kNoDeadline ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
}

void controller_task(void*) {
  const std::uint32_t boot_ms = now_ms();
  apply_event({gate::controller::EventType::kBoot,
               gate::controller::Target::kOpen,
               normalize(gate::hardware::feedback_assertions())},
              boot_ms);
  runtime_active.store(true);
  while (true) {
    const std::uint32_t before_wait = now_ms();
    process_expired(before_wait);
    RuntimeMessage message{{gate::controller::EventType::kBoot}};
    if (xQueueReceive(runtime.queue, &message, wait_ticks(before_wait)) == pdPASS) {
      const RequestResult result = apply_event(message.event, now_ms());
      if (message.reply_task != nullptr) {
        xTaskNotify(message.reply_task, static_cast<std::uint32_t>(result) + 1U,
                    eSetValueWithOverwrite);
      }
    }
  }
}

}  // namespace

esp_err_t start(const gate::config::AppConfig& config) {
  if (runtime.queue != nullptr) return ESP_ERR_INVALID_STATE;
  runtime.config = config;
  runtime.queue = xQueueCreate(kQueueLength, sizeof(RuntimeMessage));
  if (runtime.queue == nullptr) return ESP_ERR_NO_MEM;
  runtime.pulse_guard =
      new (std::nothrow) gate::controller::RelayPulseGuard(
          config.gate_operator.minimum_interval_ms);
  if (runtime.pulse_guard == nullptr) {
    release_runtime_resources();
    return ESP_ERR_NO_MEM;
  }
  esp_err_t result = gate::hardware::start_monitoring(
      config, sensor_changed, &runtime.queue);
  if (result != ESP_OK) {
    release_runtime_resources();
    return result;
  }
  if (xTaskCreate(controller_task, "gate_controller", 4096, nullptr, 6,
                  nullptr) != pdPASS) {
    gate::hardware::deactivate_all();
    // Sensor monitoring is already running and owns the callback context, so
    // its queue must remain valid. Treat this as a fail-safe terminal state;
    // no command API exists and the relay has been forced inactive.
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(kTag,
           "Serialized runtime started; authenticated bench pulses enabled");
  return ESP_OK;
}

bool active() { return runtime_active.load(); }

RequestResult send_request(const gate::controller::Event& event) {
  if (!runtime_active.load() || runtime.queue == nullptr) {
    return RequestResult::kUnavailable;
  }
  TaskHandle_t caller = xTaskGetCurrentTaskHandle();
  xTaskNotifyStateClear(caller);
  const RuntimeMessage message{event, caller};
  if (xQueueSend(runtime.queue, &message, pdMS_TO_TICKS(100)) != pdPASS) {
    return RequestResult::kBusy;
  }
  std::uint32_t notification = 0;
  if (xTaskNotifyWait(0, std::numeric_limits<std::uint32_t>::max(),
                      &notification, pdMS_TO_TICKS(500)) != pdTRUE ||
      notification == 0) {
    return RequestResult::kUnavailable;
  }
  return static_cast<RequestResult>(notification - 1U);
}

RequestResult request_bench_pulse() {
  return send_request(
      {gate::controller::EventType::kMaintenancePulseRequested});
}

RequestResult request_target(gate::controller::Target target) {
  return send_request({gate::controller::EventType::kTargetRequested, target});
}

Snapshot snapshot() {
  taskENTER_CRITICAL(&snapshot_lock);
  const gate::controller::Snapshot current = runtime.snapshot;
  taskEXIT_CRITICAL(&snapshot_lock);
  return {current.state,
          current.target,
          current.movement,
          current.stable_observation,
          current.observation_valid,
          gate::hardware::active_command(),
          current.pulse_active,
          gate::controller::obstructed(current),
          current.fault};
}

}  // namespace gate::runtime
