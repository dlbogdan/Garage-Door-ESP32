#pragma once

#include <cstdint>

namespace gate::controller {

// Pure admission state used by the runtime before committing a reducer
// transition that requests a pulse. Unsigned elapsed arithmetic is wrap-safe.
class RelayPulseGuard {
 public:
  explicit RelayPulseGuard(std::uint32_t minimum_interval_ms)
      : minimum_interval_ms_(minimum_interval_ms) {}

  bool can_start(std::uint32_t now_ms) const;
  void mark_started(std::uint32_t now_ms);
  void mark_completed();
  bool active() const { return active_; }

 private:
  std::uint32_t minimum_interval_ms_;
  std::uint32_t last_started_ms_{0};
  bool has_started_{false};
  bool active_{false};
};

}  // namespace gate::controller
