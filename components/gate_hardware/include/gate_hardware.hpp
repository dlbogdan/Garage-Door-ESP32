#pragma once

#include "app_config.hpp"
#include "esp_err.h"
#include "operator_domain.hpp"

namespace gate::hardware {

using FeedbackChangedCallback = void (*)(
    gate::controller::FeedbackAssertions assertions, void* context);

// Initializes every configured output inactive before enabling output mode,
// then starts coherent debounced feedback monitoring.
esp_err_t start_monitoring(const gate::config::AppConfig& config,
                           FeedbackChangedCallback callback = nullptr,
                           void* callback_context = nullptr);
bool monitoring_active();
gate::controller::FeedbackAssertions feedback_assertions();

// Semantic fail-safe primitives. Only the serialized runtime may call these.
esp_err_t activate_command(gate::controller::ActuatorCommand command);
esp_err_t deactivate_all();
gate::controller::ActuatorCommand active_command();

}  // namespace gate::hardware
