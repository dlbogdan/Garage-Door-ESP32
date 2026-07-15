#pragma once

#include "app_config.hpp"
#include "esp_err.h"
#include "gate_controller.hpp"

namespace gate::runtime {

enum class RequestResult {
  kAccepted,
  kBusy,
  kUnavailable,
  kHardwareError,
};

struct Snapshot {
  gate::controller::State state;
  gate::controller::Target target;
  gate::controller::MovementDirection movement;
  gate::controller::EndpointObservation observation;
  bool observation_valid;
  gate::controller::ActuatorCommand active_command;
  bool pulse_active;
  bool obstruction;
  gate::controller::FaultReason fault;
};

// Starts the single-owner reducer runtime.
esp_err_t start(const gate::config::AppConfig& config);
bool active();

// OTA maintenance is admitted only while the actuator is inactive and the
// reducer is not moving. While active, every command API rejects requests.
bool enter_maintenance();
void leave_maintenance();
bool maintenance_active();

// Queues a state-neutral bench pulse and waits briefly for admission by the
// controller task. The caller never drives GPIO or mutates reducer state.
RequestResult request_bench_pulse();
RequestResult request_target(gate::controller::Target target);
Snapshot snapshot();

}  // namespace gate::runtime
