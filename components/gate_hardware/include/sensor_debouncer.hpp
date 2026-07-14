#pragma once

#include <cstdint>

namespace gate::hardware {

struct SensorUpdate {
  bool changed{false};
  bool active{false};
};

class SensorDebouncer {
 public:
  SensorDebouncer(bool initial_active, std::uint32_t debounce_ms,
                  std::uint32_t now_ms);

  SensorUpdate update(bool raw_active, std::uint32_t now_ms);
  bool active() const { return stable_active_; }

 private:
  bool stable_active_;
  bool candidate_active_;
  std::uint32_t candidate_since_ms_;
  std::uint32_t debounce_ms_;
};

}  // namespace gate::hardware
