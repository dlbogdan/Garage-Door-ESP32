#include <cassert>
#include <cstdint>
#include <iostream>

#include "relay_pulse_guard.hpp"

int main() {
  using gate::controller::RelayPulseGuard;

  RelayPulseGuard guard(1500);
  assert(guard.can_start(100));
  guard.mark_started(100);
  assert(guard.active());
  assert(!guard.can_start(100));
  assert(!guard.can_start(5000));  // Overlap is rejected regardless of time.
  guard.mark_completed();
  assert(!guard.active());
  assert(!guard.can_start(1599));
  assert(guard.can_start(1600));

  // Minimum-interval arithmetic remains valid across the uint32_t wrap.
  RelayPulseGuard wrapped(100);
  wrapped.mark_started(UINT32_MAX - 49);
  wrapped.mark_completed();
  assert(!wrapped.can_start(49));
  assert(wrapped.can_start(50));

  std::cout << "relay pulse guard tests passed\n";
  return 0;
}
