#include "relay_pulse_guard.hpp"

namespace gate::controller {

bool RelayPulseGuard::can_start(std::uint32_t now_ms) const {
  return !active_ &&
         (!has_started_ || now_ms - last_started_ms_ >= minimum_interval_ms_);
}

void RelayPulseGuard::mark_started(std::uint32_t now_ms) {
  active_ = true;
  has_started_ = true;
  last_started_ms_ = now_ms;
}

void RelayPulseGuard::mark_completed() { active_ = false; }

}  // namespace gate::controller
