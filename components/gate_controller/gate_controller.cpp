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
        transition->next.state = State::kOpening;
        transition->effects.start_opening_timer = true;
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
        transition->next.target = Target::kOpen;
        transition->next.state = State::kStoppedOpening;
        transition->effects.cancel_travel_timers = true;
      }
      break;
    case State::kClosing:
      if (requested == Target::kClosed) {
        transition->next.target = requested;
        transition->command_result = CommandResult::kNoChange;
      } else if (begin_pulse(transition)) {
        transition->next.target = Target::kClosed;
        transition->next.state = State::kStoppedClosing;
        transition->effects.cancel_travel_timers = true;
      }
      break;
    case State::kStoppedOpening:
      if (requested == Target::kOpen) {
        transition->next.target = Target::kOpen;
        transition->command_result = CommandResult::kNoChange;
      } else if (begin_pulse(transition)) {
        transition->next.target = Target::kClosed;
        transition->next.state = State::kClosing;
        transition->effects.start_closing_timer = true;
      }
      break;
    case State::kStoppedClosing:
      if (requested == Target::kClosed) {
        transition->next.target = Target::kClosed;
        transition->command_result = CommandResult::kNoChange;
      } else if (begin_pulse(transition)) {
        transition->next.target = Target::kOpen;
        transition->next.state = State::kOpening;
        transition->effects.start_opening_timer = true;
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
      transition.next.feedback_active = event.feedback_active;
      transition.next.pulse_active = false;
      transition.next.obstruction = false;
      transition.next.state = State::kUnknownStopped;
      transition.effects.start_feedback_stability_timer = true;
      transition.effects.cancel_travel_timers = true;
      break;
    case EventType::kTargetRequested:
      request_target(&transition, event.target);
      break;
    case EventType::kMaintenancePulseRequested:
      begin_pulse(&transition);
      break;
    case EventType::kFeedbackChanged:
      transition.next.feedback_active = event.feedback_active;
      transition.effects.start_feedback_stability_timer = true;
      break;
    case EventType::kFeedbackProvedOpen:
      transition.next.feedback_active = event.feedback_active;
      transition.next.state = State::kOpen;
      transition.next.target = Target::kOpen;
      transition.next.obstruction = false;
      transition.effects.cancel_travel_timers = true;
      break;
    case EventType::kFeedbackProvedClosed:
      transition.next.feedback_active = event.feedback_active;
      transition.next.state = State::kClosed;
      transition.next.target = Target::kClosed;
      transition.next.obstruction = false;
      transition.effects.cancel_travel_timers = true;
      break;
    case EventType::kOpeningTimerExpired:
      if (current.state == State::kOpening) {
        transition.next.state = State::kStoppedOpening;
        transition.next.obstruction = true;
      }
      break;
    case EventType::kClosingTimerExpired:
      if (current.state == State::kClosing) {
        transition.next.state = State::kStoppedClosing;
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
