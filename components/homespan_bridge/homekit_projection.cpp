#include "homekit_projection.hpp"

namespace gate::homekit {

Projection project(const gate::controller::Snapshot& snapshot) {
  int current = kCurrentStopped;
  using gate::controller::State;
  switch (snapshot.state) {
    case State::kClosed: current = kCurrentClosed; break;
    case State::kOpening: current = kCurrentOpening; break;
    case State::kOpen: current = kCurrentOpen; break;
    case State::kClosing: current = kCurrentClosing; break;
    case State::kStoppedOpening:
    case State::kStoppedClosing:
    case State::kUnknownStopped:
      current = kCurrentStopped;
      break;
  }

  return {current,
          snapshot.target == gate::controller::Target::kOpen ? kTargetOpen
                                                              : kTargetClosed,
          snapshot.obstruction};
}

}  // namespace gate::homekit
