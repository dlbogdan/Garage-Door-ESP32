#include "gate_controller.hpp"

#include <cstdlib>
#include <iostream>

using gate::controller::CommandResult;
using gate::controller::Event;
using gate::controller::EventType;
using gate::controller::Snapshot;
using gate::controller::State;
using gate::controller::Target;
using gate::controller::reduce;

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

Snapshot at(State state, Target target, bool sensor = false) {
  return Snapshot{state, target, sensor, false, false};
}

void test_boot_is_safe() {
  auto result = reduce(at(State::kOpening, Target::kOpen),
                       {EventType::kBoot, Target::kClosed, false});
  expect(result.next.state == State::kUnknownStopped,
         "inactive sensor boot must become unknown stopped");
  expect(!result.effects.start_pulse, "boot must never pulse");

  result = reduce(at(State::kOpen, Target::kOpen),
                  {EventType::kBoot, Target::kOpen, true});
  expect(result.next.state == State::kClosed,
         "active sensor boot must prove closed");
  expect(result.next.target == Target::kClosed,
         "active sensor boot must align the target to closed");
  expect(!result.effects.start_pulse, "closed boot must never pulse");
}

void test_state_target_matrix() {
  const State states[] = {State::kClosed, State::kOpening, State::kOpen,
                          State::kClosing, State::kStoppedOpening,
                          State::kStoppedClosing, State::kUnknownStopped};
  const Target targets[] = {Target::kOpen, Target::kClosed};
  for (State state : states) {
    for (Target current_target : targets) {
      for (Target requested : targets) {
        const auto result = reduce(at(state, current_target, state == State::kClosed),
                                   {EventType::kTargetRequested, requested});
        expect(!result.effects.start_pulse || result.command_result == CommandResult::kAccepted,
               "only accepted commands may pulse");
        const bool stopped = state == State::kStoppedOpening ||
                             state == State::kStoppedClosing;
        const bool aligned = (state == State::kClosed && requested == Target::kClosed) ||
                             ((state == State::kOpen || state == State::kOpening) &&
                              requested == Target::kOpen) ||
                             (state == State::kClosing && requested == Target::kClosed) ||
                             (state == State::kUnknownStopped && requested == current_target);
        if (stopped || !aligned) {
          expect(result.command_result == CommandResult::kAccepted,
                 "actionable target must be accepted when idle");
          expect(result.effects.start_pulse,
                 "actionable target must emit exactly one pulse effect");
        } else {
          expect(result.command_result == CommandResult::kNoChange,
                 "aligned target must be idempotent");
          expect(!result.effects.start_pulse, "aligned target must not pulse");
        }
      }
    }
  }
}

void test_busy_rejection_has_no_delayed_action() {
  auto snapshot = at(State::kOpen, Target::kOpen);
  snapshot.pulse_active = true;
  const auto rejected = reduce(snapshot,
                               {EventType::kTargetRequested, Target::kClosed});
  expect(rejected.command_result == CommandResult::kRejectedBusy,
         "busy command must be rejected");
  expect(!rejected.effects.start_pulse, "busy command must not pulse");
  expect(rejected.next.target == Target::kOpen,
         "busy command must not alter the effective target");
  const auto completed = reduce(rejected.next, {EventType::kPulseCompleted});
  expect(!completed.effects.start_pulse,
         "pulse completion must not replay a rejected command");
}

void test_maintenance_pulse_does_not_change_motion_state() {
  const auto current = at(State::kOpen, Target::kOpen);
  const auto result = reduce(current, {EventType::kMaintenancePulseRequested});
  expect(result.command_result == CommandResult::kAccepted,
         "maintenance pulse must be accepted when idle");
  expect(result.effects.start_pulse && result.next.pulse_active,
         "maintenance request must emit one pulse effect");
  expect(result.next.state == current.state && result.next.target == current.target,
         "maintenance pulse must not infer motion or change target");
}

void test_sensor_and_timers() {
  auto result = reduce(at(State::kClosed, Target::kClosed, true),
                       {EventType::kSensorBecameInactive});
  expect(result.next.state == State::kOpening,
         "external sensor release must infer opening");
  expect(result.effects.start_opening_timer,
         "external opening must start opening timer");
  expect(!result.effects.start_pulse, "sensor event must not pulse");

  result = reduce(at(State::kOpening, Target::kOpen),
                  {EventType::kOpeningTimerExpired});
  expect(result.next.state == State::kOpen,
         "opening timeout must infer open");

  result = reduce(at(State::kClosing, Target::kClosed),
                  {EventType::kClosingTimerExpired});
  expect(result.next.state == State::kStoppedClosing && result.next.obstruction,
         "close timeout must stop and set obstruction");

  result = reduce(result.next, {EventType::kSensorBecameActive});
  expect(result.next.state == State::kClosed && !result.next.obstruction,
         "closed sensor must take authority and clear obstruction");
  expect(result.effects.cancel_travel_timers,
         "closed sensor must cancel travel timers");
}
}  // namespace

int main() {
  test_boot_is_safe();
  test_state_target_matrix();
  test_busy_rejection_has_no_delayed_action();
  test_maintenance_pulse_does_not_change_motion_state();
  test_sensor_and_timers();
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All gate controller tests passed\n";
  return EXIT_SUCCESS;
}
