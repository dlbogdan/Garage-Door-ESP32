#include "app_config.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using gate::config::AppConfig;
using gate::config::ValidationError;

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

AppConfig valid_config() {
  AppConfig config;
  config.wifi.ssid = "TestNetwork";
  config.wifi.password = "test-password";
  config.access_point.password = "setup-password";
  config.homekit.display_name = "Garage";
  config.homekit.setup_code = "48271635";
  config.homekit.setup_id = "G7T2";
  config.gate_operator.step.gpio = 25;
  config.gate_operator.single_feedback.gpio = 27;
  config.admin.salt.assign(16, 0x12);
  config.admin.password_verifier.assign(32, 0x34);
  config.admin.pbkdf2_iterations = 60000;
  return config;
}

bool has_field(const std::vector<ValidationError>& errors,
               const std::string& field) {
  return std::any_of(errors.begin(), errors.end(), [&](const auto& error) {
    return error.field == field;
  });
}

void test_complete_config_is_valid() {
  expect(gate::config::validate(valid_config()).empty(),
         "representative complete configuration must validate");
}

void test_homekit_validation() {
  auto config = valid_config();
  config.homekit.setup_code = "12345678";
  expect(has_field(gate::config::validate(config), "homekit.setupCode"),
         "trivial HomeKit setup code must be rejected");
  config = valid_config();
  config.homekit.setup_id = "abc!";
  expect(has_field(gate::config::validate(config), "homekit.setupId"),
         "lowercase/invalid Setup ID must be rejected");
}

void test_gpio_validation() {
  auto config = valid_config();
  config.gate_operator.step.gpio = 34;
  expect(has_field(gate::config::validate(config), "operator.actuators.step.gpio"),
          "input-only pin must be rejected for relay");
  config = valid_config();
  config.gate_operator.single_feedback.gpio = config.gate_operator.step.gpio;
  expect(has_field(gate::config::validate(config), "operator.feedback.single.gpio"),
          "relay and sensor collision must be rejected");
}

void test_directional_dual_configuration() {
  auto config = valid_config();
  config.gate_operator.profile = gate::config::OperatorProfile::kDirectional;
  config.gate_operator.open.gpio = 25;
  config.gate_operator.close.gpio = 26;
  config.gate_operator.feedback_topology = gate::config::FeedbackTopology::kDual;
  config.gate_operator.opened_feedback.gpio = 27;
  config.gate_operator.closed_feedback.gpio = 32;
  expect(gate::config::validate(config).empty(),
         "directional actuator with dual feedback must validate");

  config.gate_operator.close.gpio = 25;
  expect(has_field(gate::config::validate(config),
                   "operator.actuators.close.gpio"),
         "directional outputs must use distinct GPIOs");
}

void test_timing_and_admin_validation() {
  auto config = valid_config();
  config.timing.opening_ms = 2000;
  expect(has_field(gate::config::validate(config), "timing.travelMs"),
         "too-short travel time must be rejected");
  config = valid_config();
  config.admin.password_verifier.clear();
  expect(has_field(gate::config::validate(config), "admin.password"),
         "missing administrator verifier must be rejected");
}
}  // namespace

int main() {
  test_complete_config_is_valid();
  test_homekit_validation();
  test_gpio_validation();
  test_directional_dual_configuration();
  test_timing_and_admin_validation();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "All application configuration tests passed\n";
  return EXIT_SUCCESS;
}
