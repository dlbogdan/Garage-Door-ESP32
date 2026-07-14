#include "homekit_projection.hpp"

#include <cstdlib>
#include <iostream>

using namespace gate::controller;

int main() {
  const Snapshot paused_opening{State::kStoppedOpening, Target::kOpen, false,
                                true, false};
  const auto opening_projection = gate::homekit::project(paused_opening);
  if (opening_projection.current != gate::homekit::kCurrentStopped ||
      opening_projection.target != gate::homekit::kTargetOpen) {
    std::cerr << "FAIL: paused opening must publish STOPPED with retained OPEN target\n";
    return EXIT_FAILURE;
  }

  const Snapshot paused_closing{State::kStoppedClosing, Target::kClosed, false,
                                true, false};
  const auto closing_projection = gate::homekit::project(paused_closing);
  if (closing_projection.current != gate::homekit::kCurrentStopped ||
      closing_projection.target != gate::homekit::kTargetClosed) {
    std::cerr << "FAIL: paused closing must publish STOPPED with retained CLOSED target\n";
    return EXIT_FAILURE;
  }

  std::cout << "All HomeKit projection tests passed\n";
  return EXIT_SUCCESS;
}
