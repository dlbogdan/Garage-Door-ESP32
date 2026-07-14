#include "homekit_projection.hpp"

#include <cstdlib>
#include <iostream>

using namespace gate::controller;

int main() {
  Snapshot paused_opening;
  paused_opening.state = State::kStoppedOpening;
  paused_opening.target = Target::kOpen;
  paused_opening.last_movement = MovementDirection::kOpening;
  const auto opening_projection = gate::homekit::project(paused_opening);
  if (opening_projection.current != gate::homekit::kCurrentStopped ||
      opening_projection.target != gate::homekit::kTargetOpen) {
    std::cerr << "FAIL: paused opening must publish STOPPED with retained OPEN target\n";
    return EXIT_FAILURE;
  }

  Snapshot paused_closing;
  paused_closing.state = State::kStoppedClosing;
  paused_closing.target = Target::kClosed;
  paused_closing.last_movement = MovementDirection::kClosing;
  const auto closing_projection = gate::homekit::project(paused_closing);
  if (closing_projection.current != gate::homekit::kCurrentStopped ||
      closing_projection.target != gate::homekit::kTargetClosed) {
    std::cerr << "FAIL: paused closing must publish STOPPED with retained CLOSED target\n";
    return EXIT_FAILURE;
  }

  Snapshot contradiction;
  contradiction.state = State::kUnknownStopped;
  contradiction.fault = FaultReason::kFeedbackContradiction;
  const auto contradiction_projection = gate::homekit::project(contradiction);
  if (contradiction_projection.current != gate::homekit::kCurrentStopped ||
      !contradiction_projection.obstruction) {
    std::cerr << "FAIL: contradiction must publish STOPPED plus obstruction\n";
    return EXIT_FAILURE;
  }

  std::cout << "All HomeKit projection tests passed\n";
  return EXIT_SUCCESS;
}
