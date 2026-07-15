#include "gate_controller.hpp"

namespace gate::controller {
namespace {

struct StrategyDecision {
  State state;
  Target target;
  MovementDirection movement;
  MovementDirection last_movement;
  ActuatorCommand command{ActuatorCommand::kNone};
  bool start_opening_timer{false};
  bool start_closing_timer{false};
  bool cancel_travel_timers{false};
  CommandResult result{CommandResult::kNoChange};
};

StrategyDecision sequential_decision(const Snapshot& current, Target requested) {
  StrategyDecision decision{current.state, current.target, current.movement,
                            current.last_movement};
  switch (current.state) {
    case State::kClosed:
      if (requested == Target::kOpen) {
        decision = {State::kOpening, Target::kOpen,
                    MovementDirection::kOpening, MovementDirection::kOpening,
                    ActuatorCommand::kStep, true, false, false,
                    CommandResult::kAccepted};
      } else {
        decision.target = Target::kClosed;
      }
      break;
    case State::kOpen:
      if (requested == Target::kClosed) {
        decision = {State::kClosing, Target::kClosed,
                    MovementDirection::kClosing, MovementDirection::kClosing,
                    ActuatorCommand::kStep, false, true, false,
                    CommandResult::kAccepted};
      } else {
        decision.target = Target::kOpen;
      }
      break;
    case State::kOpening:
      if (requested == Target::kClosed) {
        decision = {State::kStoppedOpening, Target::kOpen,
                    MovementDirection::kNone, MovementDirection::kOpening,
                    ActuatorCommand::kStep, false, false, true,
                    CommandResult::kAccepted};
      }
      break;
    case State::kClosing:
      if (requested == Target::kOpen) {
        decision = {State::kStoppedClosing, Target::kClosed,
                    MovementDirection::kNone, MovementDirection::kClosing,
                    ActuatorCommand::kStep, false, false, true,
                    CommandResult::kAccepted};
      }
      break;
    case State::kStoppedOpening:
      if (requested == Target::kClosed) {
        decision = {State::kClosing, Target::kClosed,
                    MovementDirection::kClosing, MovementDirection::kClosing,
                    ActuatorCommand::kStep, false, true, false,
                    CommandResult::kAccepted};
      }
      break;
    case State::kStoppedClosing:
      if (requested == Target::kOpen) {
        decision = {State::kOpening, Target::kOpen,
                    MovementDirection::kOpening, MovementDirection::kOpening,
                    ActuatorCommand::kStep, true, false, false,
                    CommandResult::kAccepted};
      }
      break;
    case State::kUnknownStopped:
      if (requested != current.target) {
        decision.command = ActuatorCommand::kStep;
        decision.target = requested;
        decision.result = CommandResult::kAccepted;
      }
      break;
  }
  return decision;
}

StrategyDecision directional_decision(const Snapshot& current,
                                      Target requested) {
  StrategyDecision decision{current.state, current.target, current.movement,
                            current.last_movement};
  if ((current.state == State::kOpen && requested == Target::kOpen) ||
      (current.state == State::kClosed && requested == Target::kClosed) ||
      (current.state == State::kOpening && requested == Target::kOpen) ||
      (current.state == State::kClosing && requested == Target::kClosed)) {
    decision.target = requested;
    return decision;
  }
  if (requested == Target::kOpen) {
    decision = {State::kOpening, Target::kOpen, MovementDirection::kOpening,
                MovementDirection::kOpening, ActuatorCommand::kOpen, true,
                false, current.movement == MovementDirection::kClosing,
                CommandResult::kAccepted};
  } else {
    decision = {State::kClosing, Target::kClosed, MovementDirection::kClosing,
                MovementDirection::kClosing, ActuatorCommand::kClose, false,
                true, current.movement == MovementDirection::kOpening,
                CommandResult::kAccepted};
  }
  return decision;
}

void apply_decision(Transition* transition, const StrategyDecision& decision) {
  transition->next.state = decision.state;
  transition->next.target = decision.target;
  transition->next.movement = decision.movement;
  transition->next.last_movement = decision.last_movement;
  transition->effects.actuator_command = decision.command;
  transition->effects.start_opening_timer = decision.start_opening_timer;
  transition->effects.start_closing_timer = decision.start_closing_timer;
  transition->effects.cancel_travel_timers = decision.cancel_travel_timers;
  transition->command_result = decision.result;
  if (decision.command != ActuatorCommand::kNone) {
    transition->next.pulse_active = true;
  }
}

}  // namespace

Transition reduce(const Snapshot& current, const Event& event,
                  const ReducerContext& context) {
  Transition transition{current, {}, CommandResult::kNotACommand};
  switch (event.type) {
    case EventType::kBoot:
      transition.next = {};
      transition.next.target = current.target;
      transition.next.pending_observation = event.observation;
      transition.effects.start_feedback_stability_timer = true;
      transition.effects.cancel_travel_timers = true;
      break;
    case EventType::kTargetRequested: {
      if (current.pulse_active) {
        transition.command_result = CommandResult::kRejectedBusy;
        break;
      }
      if (current.fault == FaultReason::kFeedbackContradiction ||
          current.fault == FaultReason::kDecoderFault) {
        transition.command_result = CommandResult::kRejectedBusy;
        break;
      }
      const StrategyDecision decision =
          context.profile == OperatorProfile::kDirectional
              ? directional_decision(current, event.target)
              : sequential_decision(current, event.target);
      if (!supports(context.capabilities, decision.command)) {
        transition.command_result = CommandResult::kRejectedBusy;
        break;
      }
      apply_decision(&transition, decision);
      break;
    }
    case EventType::kMaintenancePulseRequested:
      if (current.pulse_active ||
          current.fault == FaultReason::kFeedbackContradiction ||
          current.fault == FaultReason::kDecoderFault) {
        transition.command_result = CommandResult::kRejectedBusy;
      } else {
        const ActuatorCommand command =
            context.profile == OperatorProfile::kSequential
                ? ActuatorCommand::kStep
                : (event.target == Target::kOpen ? ActuatorCommand::kOpen
                                                  : ActuatorCommand::kClose);
        if (!supports(context.capabilities, command)) {
          transition.command_result = CommandResult::kRejectedBusy;
        } else {
          transition.next.pulse_active = true;
          transition.effects.actuator_command = command;
          transition.command_result = CommandResult::kAccepted;
        }
      }
      break;
    case EventType::kObservationChanged:
      transition.next.pending_observation = event.observation;
      transition.effects.start_feedback_stability_timer = true;
      break;
    case EventType::kObservationStable:
      transition.next.pending_observation = event.observation;
      transition.next.stable_observation = event.observation;
      transition.next.observation_valid = true;
      transition.effects.cancel_travel_timers = true;
      if (event.observation == EndpointObservation::kOpened) {
        transition.next.state = State::kOpen;
        transition.next.target = Target::kOpen;
        transition.next.movement = MovementDirection::kNone;
        if (current.fault == FaultReason::kDecodedObstruction ||
            current.fault == FaultReason::kTravelTimeout) {
          transition.next.fault = FaultReason::kNone;
        }
      } else if (event.observation == EndpointObservation::kClosed) {
        transition.next.state = State::kClosed;
        transition.next.target = Target::kClosed;
        transition.next.movement = MovementDirection::kNone;
        if (current.fault == FaultReason::kDecodedObstruction ||
            current.fault == FaultReason::kTravelTimeout) {
          transition.next.fault = FaultReason::kNone;
        }
      } else if (event.observation == EndpointObservation::kContradictory) {
        transition.next.state =
            current.last_movement == MovementDirection::kOpening
                ? State::kStoppedOpening
                : current.last_movement == MovementDirection::kClosing
                      ? State::kStoppedClosing
                      : State::kUnknownStopped;
        transition.next.movement = MovementDirection::kNone;
        transition.next.fault = FaultReason::kFeedbackContradiction;
      } else if (current.fault == FaultReason::kFeedbackContradiction) {
        transition.next.state = State::kUnknownStopped;
        transition.next.movement = MovementDirection::kNone;
        transition.next.fault = FaultReason::kNone;
      } else {
        // BETWEEN is authoritative non-endpoint evidence but does not terminate
        // a locally initiated movement or invent external direction.
        transition.effects.cancel_travel_timers = false;
      }
      break;
    case EventType::kExternalOpening:
      transition.next.state = State::kOpening;
      transition.next.target = Target::kOpen;
      transition.next.movement = MovementDirection::kOpening;
      transition.next.last_movement = MovementDirection::kOpening;
      transition.effects.start_opening_timer = true;
      break;
    case EventType::kExternalClosing:
      transition.next.state = State::kClosing;
      transition.next.target = Target::kClosed;
      transition.next.movement = MovementDirection::kClosing;
      transition.next.last_movement = MovementDirection::kClosing;
      transition.effects.start_closing_timer = true;
      break;
    case EventType::kExternalStopped:
      transition.next.state =
          current.movement == MovementDirection::kOpening
              ? State::kStoppedOpening
              : current.movement == MovementDirection::kClosing
                    ? State::kStoppedClosing
                    : State::kUnknownStopped;
      transition.next.movement = MovementDirection::kNone;
      transition.effects.cancel_travel_timers = true;
      break;
    case EventType::kDecoderObstructed:
      transition.next.fault = FaultReason::kDecodedObstruction;
      break;
    case EventType::kDecoderHealthy:
      if (current.fault == FaultReason::kDecodedObstruction ||
          current.fault == FaultReason::kDecoderFault) {
        transition.next.fault = FaultReason::kNone;
      }
      break;
    case EventType::kDecoderFault:
      transition.next.state = State::kUnknownStopped;
      transition.next.movement = MovementDirection::kNone;
      transition.next.fault = FaultReason::kDecoderFault;
      transition.effects.cancel_travel_timers = true;
      break;
    case EventType::kOpeningTimerExpired:
      if (current.state == State::kOpening) {
        transition.next.state = State::kStoppedOpening;
        transition.next.movement = MovementDirection::kNone;
        transition.next.last_movement = MovementDirection::kOpening;
        transition.next.fault = FaultReason::kTravelTimeout;
      }
      break;
    case EventType::kClosingTimerExpired:
      if (current.state == State::kClosing) {
        transition.next.state = State::kStoppedClosing;
        transition.next.movement = MovementDirection::kNone;
        transition.next.last_movement = MovementDirection::kClosing;
        transition.next.fault = FaultReason::kTravelTimeout;
      }
      break;
    case EventType::kPulseCompleted:
      transition.next.pulse_active = false;
      break;
    case EventType::kObstructionAcknowledged:
      if (current.state != State::kClosing &&
          current.fault != FaultReason::kFeedbackContradiction) {
        transition.next.fault = FaultReason::kNone;
      }
      break;
  }
  return transition;
}

bool obstructed(const Snapshot& snapshot) {
  return snapshot.fault != FaultReason::kNone;
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

const char* to_string(FaultReason fault) {
  switch (fault) {
    case FaultReason::kNone: return "NONE";
    case FaultReason::kTravelTimeout: return "TRAVEL_TIMEOUT";
    case FaultReason::kFeedbackContradiction: return "FEEDBACK_CONTRADICTION";
    case FaultReason::kDecodedObstruction: return "DECODED_OBSTRUCTION";
    case FaultReason::kDecoderFault: return "DECODER_FAULT";
  }
  return "UNKNOWN";
}

}  // namespace gate::controller
