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

Snapshot at(State state, Target target, bool feedback = false) {
  return Snapshot{state, target, feedback, false, false};
}

void test_boot_waits_for_stable_feedback() {
  const auto result = reduce(at(State::kOpening, Target::kOpen),
                             {EventType::kBoot, Target::kOpen, true});
  expect(result.next.state == State::kUnknownStopped,
         "boot must remain unknown until feedback stability expires");
  expect(result.effects.start_feedback_stability_timer,
         "boot must start endpoint stability timing");
  expect(!result.effects.start_pulse, "boot must never pulse");
}

void test_endpoint_commands() {
  auto result = reduce(at(State::kClosed, Target::kClosed),
                       {EventType::kTargetRequested, Target::kOpen});
  expect(result.next.state == State::kOpening &&
             result.next.target == Target::kOpen,
         "open request from closed must report opening");
  expect(result.effects.start_pulse && result.effects.start_opening_timer,
         "open request must emit one pulse and arm timeout");

  result = reduce(at(State::kOpen, Target::kOpen),
                  {EventType::kTargetRequested, Target::kClosed});
  expect(result.next.state == State::kClosing &&
             result.next.target == Target::kClosed,
         "close request from open must report closing");
  expect(result.effects.start_pulse && result.effects.start_closing_timer,
         "close request must emit one pulse and arm timeout");
}

void test_pause_then_reverse_requires_two_commands() {
  auto paused = reduce(at(State::kOpening, Target::kOpen),
                       {EventType::kTargetRequested, Target::kClosed});
  expect(paused.next.state == State::kStoppedOpening,
         "opposite request while opening must pause");
  expect(paused.next.target == Target::kOpen,
         "paused opening must retain OPEN target for a second CLOSED command");
  expect(paused.effects.start_pulse && paused.effects.cancel_travel_timers,
         "pause must use exactly one pulse and cancel timeout");

  paused.next.pulse_active = false;
  auto reversed = reduce(paused.next,
                         {EventType::kTargetRequested, Target::kClosed});
  expect(reversed.next.state == State::kClosing &&
             reversed.next.target == Target::kClosed,
         "second closed request must reverse into closing");
  expect(reversed.effects.start_pulse && reversed.effects.start_closing_timer,
         "reverse must use exactly one new pulse");

  paused = reduce(at(State::kClosing, Target::kClosed),
                  {EventType::kTargetRequested, Target::kOpen});
  expect(paused.next.state == State::kStoppedClosing &&
             paused.next.target == Target::kClosed,
         "opposite request while closing must pause and retain CLOSED target");
  paused.next.pulse_active = false;
  reversed = reduce(paused.next,
                    {EventType::kTargetRequested, Target::kOpen});
  expect(reversed.next.state == State::kOpening &&
             reversed.next.target == Target::kOpen,
         "second open request must reverse into opening");
}

void test_feedback_is_authoritative_only_after_proof_event() {
  auto moving = at(State::kOpening, Target::kOpen);
  auto result = reduce(moving, {EventType::kFeedbackChanged,
                                Target::kOpen, true});
  expect(result.next.state == State::kOpening,
         "raw/debounced blink edge must not change movement state");
  expect(result.effects.start_feedback_stability_timer,
         "feedback edge must restart endpoint stability timing");

  result = reduce(result.next, {EventType::kFeedbackProvedOpen,
                                Target::kOpen, true});
  expect(result.next.state == State::kOpen &&
             result.next.target == Target::kOpen,
         "stable open feedback must prove OPEN");
  expect(result.effects.cancel_travel_timers,
         "proved endpoint must cancel timeout");

  result = reduce(at(State::kUnknownStopped, Target::kOpen),
                  {EventType::kFeedbackProvedClosed,
                   Target::kClosed, false});
  expect(result.next.state == State::kClosed &&
             result.next.target == Target::kClosed,
         "stable closed feedback must prove CLOSED even after external movement");
}

void test_timeouts_stop_and_obstruct_without_pulse() {
  auto result = reduce(at(State::kOpening, Target::kOpen),
                       {EventType::kOpeningTimerExpired});
  expect(result.next.state == State::kStoppedOpening && result.next.obstruction,
         "opening timeout must stop and set obstruction");
  expect(result.next.target == Target::kOpen && !result.effects.start_pulse,
         "timeout must retain destination and never pulse");

  result = reduce(at(State::kClosing, Target::kClosed),
                  {EventType::kClosingTimerExpired});
  expect(result.next.state == State::kStoppedClosing && result.next.obstruction,
         "closing timeout must stop and set obstruction");
  expect(result.next.target == Target::kClosed && !result.effects.start_pulse,
         "closing timeout must retain destination and never pulse");
}

void test_busy_rejection_has_no_delayed_action() {
  auto snapshot = at(State::kOpen, Target::kOpen);
  snapshot.pulse_active = true;
  const auto rejected = reduce(snapshot,
                               {EventType::kTargetRequested, Target::kClosed});
  expect(rejected.command_result == CommandResult::kRejectedBusy,
         "busy command must be rejected");
  expect(!rejected.effects.start_pulse && rejected.next.target == Target::kOpen,
         "busy command must not alter target or pulse");
  const auto completed = reduce(rejected.next, {EventType::kPulseCompleted});
  expect(!completed.effects.start_pulse,
         "pulse completion must not replay rejected command");
}
}  // namespace

int main() {
  test_boot_waits_for_stable_feedback();
  test_endpoint_commands();
  test_pause_then_reverse_requires_two_commands();
  test_feedback_is_authoritative_only_after_proof_event();
  test_timeouts_stop_and_obstruct_without_pulse();
  test_busy_rejection_has_no_delayed_action();
  if (failures) return EXIT_FAILURE;
  std::cout << "All gate controller tests passed\n";
  return EXIT_SUCCESS;
}
