#pragma once

#include <array>

#include "app_config.hpp"
#include "esp_err.h"
#include "operator_domain.hpp"

namespace gate::hardware {

using FeedbackChangedCallback = void (*)(
    gate::controller::FeedbackAssertions assertions, void* context);

struct FeedbackLevel final {
  gate::signal_decoder::InputId id{0};
  bool level{false};
};

struct FeedbackSample final {
  std::array<FeedbackLevel, gate::signal_decoder::DecoderLimits::kMaxInputs>
      levels{};
  std::uint8_t count{0};
  std::uint32_t timestamp_ms{0};
};

using FeedbackSampleCallback = void (*)(const FeedbackSample& sample,
                                        void* context);

// Initializes every configured output inactive before enabling output mode,
// then starts coherent debounced feedback monitoring.
esp_err_t start_monitoring(const gate::config::AppConfig& config,
                           FeedbackChangedCallback callback = nullptr,
                           void* callback_context = nullptr,
                           FeedbackSampleCallback sample_callback = nullptr);
bool monitoring_active();
gate::controller::FeedbackAssertions feedback_assertions();
FeedbackSample feedback_sample();

// Semantic fail-safe primitives. Only the serialized runtime may call these.
esp_err_t activate_command(gate::controller::ActuatorCommand command);
esp_err_t deactivate_all();
gate::controller::ActuatorCommand active_command();

}  // namespace gate::hardware
