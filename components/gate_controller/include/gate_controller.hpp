#pragma once

#include <cstdint>

#include "operator_domain.hpp"

namespace gate::controller {

enum class State : std::uint8_t {
  kClosed,
  kOpening,
  kOpen,
  kClosing,
  kStoppedOpening,
  kStoppedClosing,
  kUnknownStopped,
};

enum class Target : std::uint8_t { kOpen, kClosed };

enum class EventType : std::uint8_t {
  kBoot,
  kTargetRequested,
  kMaintenancePulseRequested,
  kObservationChanged,
  kObservationStable,
  kExternalOpening,
  kExternalClosing,
  kExternalStopped,
  kDecoderObstructed,
  kDecoderHealthy,
  kDecoderFault,
  kOpeningTimerExpired,
  kClosingTimerExpired,
  kPulseCompleted,
  kObstructionAcknowledged,
};

enum class FaultReason : std::uint8_t {
  kNone,
  kTravelTimeout,
  kFeedbackContradiction,
  kDecodedObstruction,
  kDecoderFault,
};

enum class CommandResult : std::uint8_t {
  kNotACommand,
  kAccepted,
  kNoChange,
  kRejectedBusy,
};

struct Snapshot {
  State state{State::kUnknownStopped};
  Target target{Target::kOpen};
  MovementDirection movement{MovementDirection::kNone};
  MovementDirection last_movement{MovementDirection::kNone};
  EndpointObservation pending_observation{EndpointObservation::kBetween};
  EndpointObservation stable_observation{EndpointObservation::kBetween};
  bool observation_valid{false};
  bool pulse_active{false};
  FaultReason fault{FaultReason::kNone};
};

struct Event {
  EventType type;
  Target target{Target::kOpen};
  EndpointObservation observation{EndpointObservation::kBetween};
};

struct Effects {
  ActuatorCommand actuator_command{ActuatorCommand::kNone};
  bool start_opening_timer{false};
  bool start_closing_timer{false};
  bool start_feedback_stability_timer{false};
  bool cancel_travel_timers{false};
};

struct ReducerContext {
  OperatorProfile profile{OperatorProfile::kSequential};
  OperatorCapabilities capabilities{true, false, false, false};
};

struct Transition {
  Snapshot next;
  Effects effects;
  CommandResult command_result{CommandResult::kNotACommand};
};

Transition reduce(const Snapshot& current, const Event& event,
                  const ReducerContext& context = {});
bool obstructed(const Snapshot& snapshot);
const char* to_string(State state);
const char* to_string(Target target);
const char* to_string(FaultReason fault);

}  // namespace gate::controller
