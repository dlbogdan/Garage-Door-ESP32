#include "gate_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
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
  bool has_feedback_sample{false};
  gate::hardware::FeedbackSample feedback_sample{};
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
  gate::signal_decoder::CompiledDecoder compiled_decoder;
  std::unique_ptr<gate::signal_decoder::SignalDecoder> decoder;
  std::uint32_t decoder_generation{0};
  gate::signal_decoder::DecoderResult decoder_result{};
  gate::signal_decoder::DecoderDiagnostics decoder_diagnostics{};
};

RuntimeState runtime;
std::atomic_bool runtime_active{false};
std::atomic_bool maintenance{false};
portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

RequestResult apply_event(const gate::controller::Event& event,
                          std::uint32_t now);

const char* event_name(gate::controller::EventType type) {
  using EventType = gate::controller::EventType;
  switch (type) {
    case EventType::kBoot: return "BOOT";
    case EventType::kTargetRequested: return "TARGET_REQUESTED";
    case EventType::kMaintenancePulseRequested: return "MAINTENANCE_PULSE";
    case EventType::kObservationChanged: return "OBSERVATION_CHANGED";
    case EventType::kObservationStable: return "OBSERVATION_STABLE";
    case EventType::kExternalOpening: return "EXTERNAL_OPENING";
    case EventType::kExternalClosing: return "EXTERNAL_CLOSING";
    case EventType::kExternalStopped: return "EXTERNAL_STOPPED";
    case EventType::kDecoderObstructed: return "DECODER_OBSTRUCTED";
    case EventType::kDecoderHealthy: return "DECODER_HEALTHY";
    case EventType::kDecoderFault: return "DECODER_FAULT";
    case EventType::kOpeningTimerExpired: return "OPENING_TIMER_EXPIRED";
    case EventType::kClosingTimerExpired: return "CLOSING_TIMER_EXPIRED";
    case EventType::kPulseCompleted: return "PULSE_COMPLETED";
    case EventType::kObstructionAcknowledged: return "OBSTRUCTION_ACKNOWLEDGED";
  }
  return "UNKNOWN";
}

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

void decoded_sample_changed(const gate::hardware::FeedbackSample& sample,
                            void* context) {
  auto* queue = static_cast<QueueHandle_t*>(context);
  // Queue the exact coherent sample that caused the wake-up. Reading a shared
  // "latest" sample here would coalesce multiple edges while the owner task is
  // busy and corrupt periodic timing evidence.
  RuntimeMessage message{{gate::controller::EventType::kDecoderHealthy}};
  message.has_feedback_sample = true;
  message.feedback_sample = sample;
  if (xQueueSend(*queue, &message, portMAX_DELAY) != pdPASS) {
    ESP_LOGE(kTag, "Could not enqueue decoded feedback sample");
  }
}

void apply_decoder_result(std::uint32_t now) {
  if (runtime.decoder == nullptr) return;
  const auto& decoded = runtime.decoder->result();
  taskENTER_CRITICAL(&snapshot_lock);
  runtime.decoder_result = decoded;
  runtime.decoder_diagnostics = runtime.decoder->diagnostics(now);
  taskEXIT_CRITICAL(&snapshot_lock);
  if (decoded.generation == runtime.decoder_generation) return;
  runtime.decoder_generation = decoded.generation;
  ESP_LOGI(kTag,
           "Decoder generation=%lu health=%u positionValid=%u position=%u "
           "movementValid=%u movement=%u obstructed=%u predicates=%u rules=%u",
           static_cast<unsigned long>(decoded.generation),
           static_cast<unsigned>(decoded.health),
           static_cast<unsigned>(decoded.position_valid),
           static_cast<unsigned>(decoded.position),
           static_cast<unsigned>(decoded.movement_valid),
           static_cast<unsigned>(decoded.movement),
           static_cast<unsigned>(decoded.obstructed),
           static_cast<unsigned>(runtime.decoder_diagnostics.predicate_count),
           static_cast<unsigned>(runtime.decoder_diagnostics.rule_count));
  for (std::uint8_t i = 0; i < runtime.decoder_diagnostics.predicate_count; ++i) {
    const auto& diagnostic = runtime.decoder_diagnostics.predicates[i];
    ESP_LOGI(kTag,
             "Decoder predicate[%u]: value=%u evidenceValid=%u ageMs=%lu "
             "latestIntervalMs=%lu qualifyingEdges=%u",
             static_cast<unsigned>(i), static_cast<unsigned>(diagnostic.value),
             static_cast<unsigned>(diagnostic.evidence_valid),
             static_cast<unsigned long>(diagnostic.evidence_age_ms),
             static_cast<unsigned long>(diagnostic.latest_interval_ms),
             static_cast<unsigned>(diagnostic.qualifying_edge_count));
  }
  for (std::uint8_t i = 0; i < runtime.decoder_diagnostics.rule_count; ++i) {
    const auto& diagnostic = runtime.decoder_diagnostics.rules[i];
    ESP_LOGI(kTag,
             "Decoder rule[%u]: id=%u expression=%u asserted=%u phase=%u "
             "matchAgeMs=%lu",
             static_cast<unsigned>(i), static_cast<unsigned>(diagnostic.id),
             static_cast<unsigned>(diagnostic.expression_value),
             static_cast<unsigned>(diagnostic.output_asserted),
             static_cast<unsigned>(diagnostic.movement_phase),
             static_cast<unsigned long>(diagnostic.match_age_ms));
  }
  if (decoded.health == gate::signal_decoder::DecoderHealth::kAmbiguousPosition ||
      decoded.health == gate::signal_decoder::DecoderHealth::kAmbiguousMovement ||
      decoded.health == gate::signal_decoder::DecoderHealth::kMonitoringFailed) {
    apply_event({gate::controller::EventType::kDecoderFault}, now);
    return;
  }
  if (decoded.position_valid) {
    apply_event({gate::controller::EventType::kObservationStable,
                 gate::controller::Target::kOpen,
                 decoded.position == gate::signal_decoder::PositionValue::kOpened
                     ? gate::controller::EndpointObservation::kOpened
                     : gate::controller::EndpointObservation::kClosed}, now);
  } else if (decoded.movement_valid) {
    const auto type =
        decoded.movement == gate::signal_decoder::MovementValue::kOpening
            ? gate::controller::EventType::kExternalOpening
            : decoded.movement == gate::signal_decoder::MovementValue::kClosing
                  ? gate::controller::EventType::kExternalClosing
                  : gate::controller::EventType::kExternalStopped;
    apply_event({type}, now);
  }
  apply_event({decoded.obstructed
                   ? gate::controller::EventType::kDecoderObstructed
                   : gate::controller::EventType::kDecoderHealthy}, now);
}

void consume_custom_sample(const gate::hardware::FeedbackSample& sample,
                           std::uint32_t now) {
  if (runtime.decoder == nullptr) return;
  for (std::uint8_t i = 0; i < sample.count; ++i) {
    if (!runtime.decoder->update(sample.levels[i].id, sample.levels[i].level,
                                 sample.timestamp_ms)) {
      runtime.decoder->initialize(sample.levels[i].id, sample.levels[i].level,
                                  sample.timestamp_ms);
    }
  }
  runtime.decoder->advance(now);
  apply_decoder_result(now);
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
  ESP_LOGI(kTag, "Controller event=%s state=%s target=%s observation=%s valid=%d pulse=%d fault=%s",
           event_name(event.type),
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
  if (runtime.decoder != nullptr) {
    const auto deadline = runtime.decoder->next_deadline();
    if (deadline.valid && reached(now, deadline.at)) {
      runtime.decoder->advance(now);
      apply_decoder_result(now);
    }
  }
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
  if (runtime.decoder != nullptr) {
    const auto deadline = runtime.decoder->next_deadline();
    if (deadline.valid) {
      if (reached(now, deadline.at)) return 0;
      wait_ms = std::min(wait_ms, deadline.at - now);
    }
  }
  return wait_ms == kNoDeadline ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
}

void controller_task(void*) {
  const std::uint32_t boot_ms = now_ms();
  if (runtime.decoder != nullptr) {
    apply_event({gate::controller::EventType::kBoot}, boot_ms);
    consume_custom_sample(gate::hardware::feedback_sample(), boot_ms);
  } else {
    apply_event({gate::controller::EventType::kBoot,
                 gate::controller::Target::kOpen,
                 normalize(gate::hardware::feedback_assertions())},
                boot_ms);
  }
  runtime_active.store(true);
  while (true) {
    const std::uint32_t before_wait = now_ms();
    process_expired(before_wait);
    RuntimeMessage message{{gate::controller::EventType::kBoot}};
    if (xQueueReceive(runtime.queue, &message, wait_ticks(before_wait)) == pdPASS) {
      const std::uint32_t received_at = now_ms();
      const bool custom_sample_wakeup =
          runtime.decoder != nullptr && message.has_feedback_sample;
      if (custom_sample_wakeup) {
        consume_custom_sample(message.feedback_sample, received_at);
      }
      const RequestResult result = custom_sample_wakeup
                                       ? RequestResult::kAccepted
                                       : apply_event(message.event, received_at);
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
  if (config.gate_operator.decoder.profile ==
          gate::config::FeedbackDecoderProfile::kCustomRules &&
      config.gate_operator.decoder.input_count > 0) {
    gate::signal_decoder::CompileError compile_error;
    if (!gate::signal_decoder::compile(config.gate_operator.decoder.rules,
                                       &runtime.compiled_decoder,
                                       &compile_error)) {
      return ESP_ERR_INVALID_ARG;
    }
    runtime.decoder = std::make_unique<gate::signal_decoder::SignalDecoder>(
        runtime.compiled_decoder);
  }
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
      config, runtime.decoder == nullptr ? sensor_changed : nullptr,
      &runtime.queue,
      runtime.decoder != nullptr ? decoded_sample_changed : nullptr);
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
  if (maintenance.load()) return RequestResult::kBusy;
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

bool enter_maintenance() {
  if (!runtime_active.load()) return false;
  bool expected = false;
  if (!maintenance.compare_exchange_strong(expected, true)) return false;
  const Snapshot current = snapshot();
  if (current.pulse_active ||
      current.movement != gate::controller::MovementDirection::kNone) {
    maintenance.store(false);
    return false;
  }
  if (gate::hardware::deactivate_all() != ESP_OK) {
    maintenance.store(false);
    return false;
  }
  ESP_LOGI(kTag, "Maintenance mode entered; commands interlocked");
  return true;
}

void leave_maintenance() {
  if (maintenance.exchange(false)) {
    gate::hardware::deactivate_all();
    ESP_LOGI(kTag, "Maintenance mode left");
  }
}

bool maintenance_active() { return maintenance.load(); }

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

DecoderSnapshot decoder_snapshot() {
  // Diagnostics are 840 bytes at current limits. Copying while holding the
  // critical section gives REST one coherent generation; callers must provide
  // a task stack sized for this explicitly bounded snapshot.
  static_assert(sizeof(gate::signal_decoder::DecoderDiagnostics) <= 1024,
                "Reassess REST snapshot and HTTP task stack capacity");
  taskENTER_CRITICAL(&snapshot_lock);
  const DecoderSnapshot current{runtime.decoder != nullptr,
                                runtime.decoder_result,
                                runtime.decoder_diagnostics};
  taskEXIT_CRITICAL(&snapshot_lock);
  return current;
}

}  // namespace gate::runtime
