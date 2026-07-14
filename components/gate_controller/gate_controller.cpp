#include "gate_controller.hpp"

namespace gate::controller {
namespace {

bool begin_pulse(Transition* transition) {
  if (transition->next.pulse_active) {
    transition->command_result = CommandResult::kRejectedBusy;
    return false;
  }
  transition->next.pulse_active = true;
  transition->effects.start_pulse = true;
  transition->command_result = CommandResult::kAccepted;
  return true;
}

void request_target(Transition* transition, Target requested) {
  switch (transition->next.state) {
    case State::kClosed:
      if (requested == Target::kClosed) {
        transition->next.target = requested;
        transition->command_result = CommandResult::kNoChange;
      } else if (begin_pulse(transition)) {
        transition->next.target = requested;
        transition->effects.start_sensor_release_timer = true;
      }
      break;
    case State::kOpen:
      if (requested == Target::kOpen) {
        transition->next.target = requested;
        transition->command_result = CommandResult::kNoChange;
      } else if (begin_pulse(transition)) {
        transition->next.target = requested;
        transition->next.state = State::kClosing;
        transition->effects.start_closing_timer = true;
      }
      break;
    case State::kOpening:
      if (requested == Target::kOpen) {
        transition->next.target = requested;
        transition->command_result = CommandResult::kNoChange;
      } else if (begin_pulse(transition)) {
        transition->next.target = requested;
        transition->next.state = State::kStoppedOpening;
        transition->effects.cancel_travel_timers = true;
      }
      break;
    case State::kClosing:
      if (requested == Target::kClosed) {
        transition->next.target = requested;
        transition->command_result = CommandResult::kNoChange;
      } else if (begin_pulse(transition)) {
        transition->next.target = requested;
        transition->next.state = State::kStoppedClosing;
        transition->effects.cancel_travel_timers = true;
      }
      break;
    case State::kStoppedOpening:
      if (begin_pulse(transition)) {
        transition->next.target = requested;
        transition->next.state = requested == Target::kOpen ? State::kOpening
                                                             : State::kClosing;
        transition->effects.start_opening_timer = requested == Target::kOpen;
        transition->effects.start_closing_timer = requested == Target::kClosed;
      }
      break;
    case State::kStoppedClosing:
      if (begin_pulse(transition)) {
        transition->next.target = requested;
        transition->next.state = requested == Target::kOpen ? State::kOpening
                                                             : State::kClosing;
        transition->effects.start_opening_timer = requested == Target::kOpen;
        transition->effects.start_closing_timer = requested == Target::kClosed;
      }
      break;
    case State::kUnknownStopped:
      if (requested == transition->next.target) {
        transition->command_result = CommandResult::kNoChange;
      } else if (begin_pulse(transition)) {
        transition->next.target = requested;
      }
      break;
  }
}

}  // namespace

Transition reduce(const Snapshot& current, const Event& event) {
  Transition transition{current, {}, CommandResult::kNotACommand};
  switch (event.type) {
    case EventType::kBoot:
      transition.next.sensor_active = event.sensor_active;
      transition.next.pulse_active = false;
      transition.next.obstruction = false;
      transition.next.state = event.sensor_active ? State::kClosed
                                                  : State::kUnknownStopped;
      if (event.sensor_active) transition.next.target = Target::kClosed;
      transition.effects.cancel_travel_timers = true;
      break;
    case EventType::kTargetRequested:
      request_target(&transition, event.target);
      break;
    case EventType::kMaintenancePulseRequested:
      begin_pulse(&transition);
      break;
    case EventType::kSensorBecameActive:
      transition.next.sensor_active = true;
      transition.next.state = State::kClosed;
      transition.next.obstruction = false;
      transition.effects.cancel_travel_timers = true;
      break;
    case EventType::kSensorBecameInactive:
      transition.next.sensor_active = false;
      if (current.state == State::kClosed) {
        transition.next.state = State::kOpening;
        transition.next.target = Target::kOpen;
        transition.effects.start_opening_timer = true;
      }
      break;
    case EventType::kOpeningTimerExpired:
      if (current.state == State::kOpening && !current.sensor_active) {
        transition.next.state = State::kOpen;
      }
      break;
    case EventType::kClosingTimerExpired:
      if (current.state == State::kClosing && !current.sensor_active) {
        transition.next.state = State::kStoppedClosing;
        transition.next.obstruction = true;
      }
      break;
    case EventType::kSensorReleaseTimerExpired:
      if (current.state == State::kClosed && current.sensor_active) {
        transition.next.obstruction = true;
      }
      break;
    case EventType::kPulseCompleted:
      transition.next.pulse_active = false;
      break;
    case EventType::kObstructionAcknowledged:
      if (current.state != State::kClosing) {
        transition.next.obstruction = false;
      }
      break;
  }
  return transition;
}

const char* to_string(State state) {
  switch (state) {
    case State::kClosed: return "CLOSED";
    case State::kOpening: return "OPENING";
    case State::kOpen: return "OPEN";
    case State::kClosing: return "CLOSING";
    case State::kStoppedOpening: return "STOPPED_OPENING";
    case State::kStoppedClosing: return "STOPPED_CLOSING";
    case State::kUnknownStopped: return "UNKNOWN_STOPPED";
  }
  return "UNKNOWN";
}

const char* to_string(Target target) {
  return target == Target::kOpen ? "OPEN" : "CLOSED";
}

}  // namespace gate::controller
