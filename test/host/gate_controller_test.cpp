#include "gate_controller.hpp"

#include <cstdlib>
#include <iostream>

using namespace gate::controller;

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

Snapshot at(State state, Target target) {
  Snapshot snapshot;
  snapshot.state = state;
  snapshot.target = target;
  if (state == State::kOpening) {
    snapshot.movement = MovementDirection::kOpening;
    snapshot.last_movement = MovementDirection::kOpening;
  } else if (state == State::kClosing) {
    snapshot.movement = MovementDirection::kClosing;
    snapshot.last_movement = MovementDirection::kClosing;
  } else if (state == State::kStoppedOpening) {
    snapshot.last_movement = MovementDirection::kOpening;
  } else if (state == State::kStoppedClosing) {
    snapshot.last_movement = MovementDirection::kClosing;
  }
  return snapshot;
}

ReducerContext directional() {
  return {OperatorProfile::kDirectional, {false, true, true, false}};
}

void test_boot_waits_for_stable_feedback() {
  const auto result = reduce(at(State::kOpening, Target::kOpen),
                             {EventType::kBoot, Target::kOpen,
                              EndpointObservation::kClosed});
  expect(result.next.state == State::kUnknownStopped &&
             !result.next.observation_valid,
         "boot must remain unknown until observation stability expires");
  expect(result.effects.start_feedback_stability_timer,
         "boot must start endpoint stability timing");
  expect(result.effects.actuator_command == ActuatorCommand::kNone,
         "boot must never command an actuator");
}

void test_sequential_pause_then_reverse() {
  auto opened = reduce(at(State::kClosed, Target::kClosed),
                       {EventType::kTargetRequested, Target::kOpen});
  expect(opened.next.state == State::kOpening &&
             opened.effects.actuator_command == ActuatorCommand::kStep,
         "sequential open must emit STEP and report OPENING");

  auto paused = reduce(at(State::kOpening, Target::kOpen),
                       {EventType::kTargetRequested, Target::kClosed});
  expect(paused.next.state == State::kStoppedOpening &&
             paused.next.movement == MovementDirection::kNone &&
             paused.next.last_movement == MovementDirection::kOpening,
         "opposite sequential request must atomically stop and retain history");
  expect(paused.next.target == Target::kOpen &&
             paused.effects.actuator_command == ActuatorCommand::kStep,
         "sequential pause must retain OPEN target and emit one STEP");

  paused.next.pulse_active = false;
  const auto reversed = reduce(paused.next,
                               {EventType::kTargetRequested, Target::kClosed});
  expect(reversed.next.state == State::kClosing &&
             reversed.next.target == Target::kClosed &&
             reversed.next.movement == MovementDirection::kClosing,
         "second explicit sequential request must start closing atomically");
  expect(reversed.effects.actuator_command == ActuatorCommand::kStep,
         "sequential reverse must use one new STEP");
}

void test_directional_direct_reversal() {
  const auto closing = reduce(at(State::kOpening, Target::kOpen),
                              {EventType::kTargetRequested, Target::kClosed},
                              directional());
  expect(closing.next.state == State::kClosing &&
             closing.next.target == Target::kClosed &&
             closing.next.movement == MovementDirection::kClosing,
         "directional opposite request must atomically report CLOSING");
  expect(closing.effects.actuator_command == ActuatorCommand::kClose &&
             closing.effects.start_closing_timer &&
             closing.effects.cancel_travel_timers,
         "directional reversal must emit exactly CLOSE and replace timer");
}

void test_observation_pipeline_and_contradiction() {
  auto result = reduce(at(State::kOpening, Target::kOpen),
                       {EventType::kObservationChanged, Target::kOpen,
                        EndpointObservation::kOpened});
  expect(result.next.state == State::kOpening &&
             !result.next.observation_valid,
         "pending observation must not change state");

  result = reduce(result.next,
                  {EventType::kObservationStable, Target::kOpen,
                   EndpointObservation::kOpened});
  expect(result.next.state == State::kOpen && result.next.observation_valid &&
             result.next.movement == MovementDirection::kNone,
         "stable OPENED must atomically prove endpoint");

  auto moving = at(State::kClosing, Target::kClosed);
  result = reduce(moving,
                  {EventType::kObservationStable, Target::kOpen,
                   EndpointObservation::kContradictory});
  expect(result.next.state == State::kStoppedClosing &&
             result.next.fault == FaultReason::kFeedbackContradiction &&
             result.effects.cancel_travel_timers,
         "contradiction must atomically stop, fault, and cancel travel");
  const auto blocked = reduce(result.next,
                              {EventType::kTargetRequested, Target::kOpen},
                              directional());
  expect(blocked.command_result == CommandResult::kRejectedBusy &&
             blocked.effects.actuator_command == ActuatorCommand::kNone &&
             blocked.next.state == result.next.state,
         "contradiction must reject without partially changing snapshot");
}

void test_timeout_and_busy_are_atomic() {
  const auto timed_out = reduce(at(State::kOpening, Target::kOpen),
                                {EventType::kOpeningTimerExpired});
  expect(timed_out.next.state == State::kStoppedOpening &&
             timed_out.next.movement == MovementDirection::kNone &&
             timed_out.next.fault == FaultReason::kTravelTimeout &&
             timed_out.effects.actuator_command == ActuatorCommand::kNone,
         "timeout must atomically stop/fault without command");

  auto current = at(State::kOpen, Target::kOpen);
  current.pulse_active = true;
  const auto rejected = reduce(current,
                               {EventType::kTargetRequested, Target::kClosed});
  expect(rejected.command_result == CommandResult::kRejectedBusy &&
             rejected.next.state == current.state &&
             rejected.next.target == current.target &&
             rejected.next.pulse_active == current.pulse_active,
         "busy rejection must preserve complete committed snapshot");
  const auto completed = reduce(rejected.next, {EventType::kPulseCompleted});
  expect(completed.effects.actuator_command == ActuatorCommand::kNone,
         "pulse completion must never replay rejected command");
}

void test_external_decoder_events_never_actuate() {
  const auto opening =
      reduce(at(State::kUnknownStopped, Target::kClosed),
             {EventType::kExternalOpening});
  expect(opening.next.state == State::kOpening &&
             opening.next.movement == MovementDirection::kOpening &&
             opening.effects.start_opening_timer &&
             opening.effects.actuator_command == ActuatorCommand::kNone,
         "decoded external opening must update movement without a pulse");

  const auto obstructed =
      reduce(opening.next, {EventType::kDecoderObstructed});
  expect(obstructed.next.fault == FaultReason::kDecodedObstruction &&
             obstructed.effects.actuator_command == ActuatorCommand::kNone,
         "decoded obstruction must set fault without a pulse");

  const auto endpoint =
      reduce(obstructed.next,
             {EventType::kObservationStable, Target::kOpen,
              EndpointObservation::kOpened});
  expect(endpoint.next.state == State::kOpen &&
             endpoint.next.fault == FaultReason::kNone,
         "proved endpoint must clear decoded obstruction");

  const auto fault = reduce(endpoint.next, {EventType::kDecoderFault});
  expect(fault.next.state == State::kUnknownStopped &&
             fault.next.fault == FaultReason::kDecoderFault &&
             fault.effects.cancel_travel_timers &&
             fault.effects.actuator_command == ActuatorCommand::kNone,
         "decoder ambiguity/health fault must stop and interlock without pulse");
  const auto blocked =
      reduce(fault.next, {EventType::kTargetRequested, Target::kClosed});
  expect(blocked.command_result == CommandResult::kRejectedBusy &&
             blocked.effects.actuator_command == ActuatorCommand::kNone,
         "decoder fault must interlock target commands");

  const auto still_faulted =
      reduce(fault.next,
             {EventType::kObservationStable, Target::kOpen,
              EndpointObservation::kClosed});
  expect(still_faulted.next.state == State::kClosed &&
             still_faulted.next.fault == FaultReason::kDecoderFault,
         "endpoint authority must not clear decoder ambiguity/monitoring fault");

  const auto recovered =
      reduce(still_faulted.next, {EventType::kDecoderHealthy});
  expect(recovered.next.fault == FaultReason::kNone,
         "explicit decoder health recovery must clear decoder fault");
}
}  // namespace

int main() {
  test_boot_waits_for_stable_feedback();
  test_sequential_pause_then_reverse();
  test_directional_direct_reversal();
  test_observation_pipeline_and_contradiction();
  test_timeout_and_busy_are_atomic();
  test_external_decoder_events_never_actuate();
  if (failures) return EXIT_FAILURE;
  std::cout << "All gate controller tests passed\n";
  return EXIT_SUCCESS;
}
