#include "sensor_debouncer.hpp"

namespace gate::hardware {

SensorDebouncer::SensorDebouncer(bool initial_active,
                                 std::uint32_t debounce_ms,
                                 std::uint32_t now_ms)
    : stable_active_(initial_active),
      candidate_active_(initial_active),
      candidate_since_ms_(now_ms),
      debounce_ms_(debounce_ms) {}

SensorUpdate SensorDebouncer::update(bool raw_active, std::uint32_t now_ms) {
  if (raw_active != candidate_active_) {
    candidate_active_ = raw_active;
    candidate_since_ms_ = now_ms;
    return {};
  }
  if (candidate_active_ != stable_active_ &&
      now_ms - candidate_since_ms_ >= debounce_ms_) {
    stable_active_ = candidate_active_;
    return {true, stable_active_};
  }
  return {};
}

}  // namespace gate::hardware
