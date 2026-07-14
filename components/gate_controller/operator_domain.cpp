#include "operator_domain.hpp"

namespace gate::controller {

EndpointObservation normalize_feedback(
    FeedbackTopology topology, Endpoint single_asserted_endpoint,
    const FeedbackAssertions& assertions) {
  if (topology == FeedbackTopology::kDual) {
    if (assertions.opened && assertions.closed) {
      return EndpointObservation::kContradictory;
    }
    if (assertions.opened) return EndpointObservation::kOpened;
    if (assertions.closed) return EndpointObservation::kClosed;
    return EndpointObservation::kBetween;
  }

  const bool asserted = single_asserted_endpoint == Endpoint::kOpened
                            ? assertions.opened
                            : assertions.closed;
  if (single_asserted_endpoint == Endpoint::kOpened) {
    return asserted ? EndpointObservation::kOpened
                    : EndpointObservation::kClosed;
  }
  return asserted ? EndpointObservation::kClosed
                  : EndpointObservation::kOpened;
}

const char* to_string(ActuatorCommand command) {
  switch (command) {
    case ActuatorCommand::kNone: return "NONE";
    case ActuatorCommand::kStep: return "STEP";
    case ActuatorCommand::kOpen: return "OPEN";
    case ActuatorCommand::kClose: return "CLOSE";
  }
  return "UNKNOWN";
}

const char* to_string(EndpointObservation observation) {
  switch (observation) {
    case EndpointObservation::kBetween: return "BETWEEN";
    case EndpointObservation::kOpened: return "OPENED";
    case EndpointObservation::kClosed: return "CLOSED";
    case EndpointObservation::kContradictory: return "CONTRADICTORY";
  }
  return "UNKNOWN";
}

const char* to_string(MovementDirection direction) {
  switch (direction) {
    case MovementDirection::kNone: return "NONE";
    case MovementDirection::kOpening: return "OPENING";
    case MovementDirection::kClosing: return "CLOSING";
  }
  return "UNKNOWN";
}

}  // namespace gate::controller
