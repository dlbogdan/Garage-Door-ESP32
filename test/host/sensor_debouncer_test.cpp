#include <cassert>
#include <cstdint>
#include <iostream>

#include "sensor_debouncer.hpp"

int main() {
  using gate::hardware::SensorDebouncer;

  SensorDebouncer sensor(false, 50, 100);
  assert(!sensor.active());
  assert(!sensor.update(true, 110).changed);
  assert(!sensor.update(false, 120).changed);  // Bounce resets the candidate.
  assert(!sensor.update(true, 130).changed);
  assert(!sensor.update(true, 179).changed);
  const auto closed = sensor.update(true, 180);
  assert(closed.changed && closed.active && sensor.active());
  assert(!sensor.update(true, 500).changed);

  assert(!sensor.update(false, 510).changed);
  assert(!sensor.update(false, 559).changed);
  const auto opened = sensor.update(false, 560);
  assert(opened.changed && !opened.active && !sensor.active());

  // Unsigned subtraction keeps elapsed-time checks valid across millis wrap.
  SensorDebouncer wrapped(false, 20, UINT32_MAX - 10);
  assert(!wrapped.update(true, UINT32_MAX - 5).changed);
  const auto after_wrap = wrapped.update(true, 14);
  assert(after_wrap.changed && after_wrap.active);

  std::cout << "sensor debouncer tests passed\n";
  return 0;
}
