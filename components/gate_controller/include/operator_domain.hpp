#pragma once

#include <cstdint>

namespace gate::controller {

enum class OperatorProfile : std::uint8_t { kSequential, kDirectional };
enum class FeedbackTopology : std::uint8_t { kSingle, kDual };

enum class ActuatorCommand : std::uint8_t {
  kNone,
  kStep,
  kOpen,
  kClose,
};

enum class Endpoint : std::uint8_t { kOpened, kClosed };

// Absence of a stable observation is represented separately by
// observation_valid. Hardware can produce only these four semantic results.
enum class EndpointObservation : std::uint8_t {
  kBetween,
  kOpened,
  kClosed,
  kContradictory,
};

enum class MovementDirection : std::uint8_t {
  kNone,
  kOpening,
  kClosing,
};

struct FeedbackAssertions {
  bool opened{false};
  bool closed{false};
};

struct OperatorCapabilities {
  bool supports_step{false};
  bool supports_open{false};
  bool supports_close{false};
  bool supports_dual_feedback{false};
};

constexpr bool supports(const OperatorCapabilities& capabilities,
                        ActuatorCommand command) {
  switch (command) {
    case ActuatorCommand::kNone: return true;
    case ActuatorCommand::kStep: return capabilities.supports_step;
    case ActuatorCommand::kOpen: return capabilities.supports_open;
    case ActuatorCommand::kClose: return capabilities.supports_close;
  }
  return false;
}

EndpointObservation normalize_feedback(FeedbackTopology topology,
                                       Endpoint single_asserted_endpoint,
                                       const FeedbackAssertions& assertions);

const char* to_string(ActuatorCommand command);
const char* to_string(EndpointObservation observation);
const char* to_string(MovementDirection direction);

}  // namespace gate::controller
