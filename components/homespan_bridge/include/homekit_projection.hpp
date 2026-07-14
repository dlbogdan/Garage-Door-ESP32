#pragma once

#include "gate_controller.hpp"

namespace gate::homekit {

// Standard HAP Garage Door Opener values. Keeping this projection independent
// of HomeSpan makes the controller-to-HomeKit contract host-testable.
struct Projection {
  int current;
  int target;
  bool obstruction;
};

constexpr int kCurrentOpen = 0;
constexpr int kCurrentClosed = 1;
constexpr int kCurrentOpening = 2;
constexpr int kCurrentClosing = 3;
constexpr int kCurrentStopped = 4;
constexpr int kTargetOpen = 0;
constexpr int kTargetClosed = 1;

Projection project(const gate::controller::Snapshot& snapshot);

}  // namespace gate::homekit
