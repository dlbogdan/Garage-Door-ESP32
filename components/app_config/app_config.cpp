#include "app_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace gate::config {
namespace {

void add_error(std::vector<ValidationError>* errors, const char* field,
               const char* code, const char* message) {
  errors->push_back({field, code, message});
}

bool in_range(std::uint32_t value, std::uint32_t minimum,
              std::uint32_t maximum) {
  return value >= minimum && value <= maximum;
}

}  // namespace

bool is_safe_gpio(int gpio, bool output) {
  // GPIO0 is recovery; 1/3 are serial; 6-11 are flash; 12 is a strapping pin;
  // 34-39 are input-only and lack internal pulls.
  constexpr std::array<int, 11> kForbidden{0, 1, 3, 6, 7, 8, 9, 10, 11, 12,
                                            20};
  if (gpio < 0 || gpio > 39 ||
      std::find(kForbidden.begin(), kForbidden.end(), gpio) !=
          kForbidden.end()) {
    return false;
  }
  return !output || gpio < 34;
}

bool is_valid_homekit_setup_code(const std::string& code) {
  if (code.size() != 8 ||
      !std::all_of(code.begin(), code.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
      })) {
    return false;
  }
  constexpr std::array<const char*, 12> kInvalid{
      "00000000", "11111111", "22222222", "33333333",
      "44444444", "55555555", "66666666", "77777777",
      "88888888", "99999999", "12345678", "87654321"};
  return std::none_of(kInvalid.begin(), kInvalid.end(),
                      [&code](const char* invalid) { return code == invalid; });
}

bool is_valid_homekit_setup_id(const std::string& setup_id) {
  return setup_id.size() == 4 &&
         std::all_of(setup_id.begin(), setup_id.end(), [](unsigned char value) {
           return std::isdigit(value) || (value >= 'A' && value <= 'Z');
         });
}

bool is_provisioned(const AppConfig& config) {
  return validate(config).empty();
}

std::vector<ValidationError> validate(const AppConfig& config) {
  std::vector<ValidationError> errors;
  if (config.schema_version != kSchemaVersion) {
    add_error(&errors, "schemaVersion", "unsupported", "Unsupported schema");
  }
  if (config.wifi.ssid.empty() || config.wifi.ssid.size() > 32) {
    add_error(&errors, "wifi.ssid", "length", "SSID must be 1-32 bytes");
  }
  if (config.wifi.password.size() > 63) {
    add_error(&errors, "wifi.password", "length",
              "Wi-Fi password must not exceed 63 bytes");
  }
  if (!in_range(config.wifi.connection_deadline_ms, 10000, 180000)) {
    add_error(&errors, "wifi.connectionDeadlineMs", "range",
              "Connection deadline must be 10-180 seconds");
  }
  if (config.access_point.password.size() < 8 ||
      config.access_point.password.size() > 63) {
    add_error(&errors, "accessPoint.password", "length",
              "Setup AP password must be 8-63 bytes");
  }
  if (config.homekit.display_name.empty() ||
      config.homekit.display_name.size() > 64) {
    add_error(&errors, "homekit.displayName", "length",
              "Display name must be 1-64 bytes");
  }
  if (!is_valid_homekit_setup_code(config.homekit.setup_code)) {
    add_error(&errors, "homekit.setupCode", "format",
              "Use a nontrivial eight-digit setup code");
  }
  if (!is_valid_homekit_setup_id(config.homekit.setup_id)) {
    add_error(&errors, "homekit.setupId", "format",
              "Setup ID must be four uppercase letters or digits");
  }
  if (!is_safe_gpio(config.relay.gpio, true)) {
    add_error(&errors, "relay.gpio", "unsafe", "Unsafe relay GPIO");
  }
  if (!is_safe_gpio(config.sensor.gpio, false)) {
    add_error(&errors, "sensor.gpio", "unsafe", "Unsafe sensor GPIO");
  }
  if (config.relay.gpio == config.sensor.gpio && config.relay.gpio >= 0) {
    add_error(&errors, "sensor.gpio", "collision",
              "Relay and sensor GPIOs must differ");
  }
  if (!in_range(config.relay.pulse_ms, 100, 2000)) {
    add_error(&errors, "relay.pulseMs", "range", "Pulse must be 100-2000 ms");
  }
  if (!in_range(config.relay.minimum_interval_ms, 500, 10000)) {
    add_error(&errors, "relay.minimumIntervalMs", "range",
              "Minimum interval must be 500-10000 ms");
  }
  if (!in_range(config.sensor.debounce_ms, 10, 500)) {
    add_error(&errors, "sensor.debounceMs", "range",
              "Debounce must be 10-500 ms");
  }
  if (!in_range(config.sensor.endpoint_stability_ms, 1000, 10000)) {
    add_error(&errors, "sensor.endpointStabilityMs", "range",
              "Endpoint stability must be 1-10 seconds");
  }
  if (config.sensor.gpio >= 34 && config.sensor.pull != SensorPull::kNone) {
    add_error(&errors, "sensor.pull", "unsupported",
              "GPIO34-39 do not support internal pulls");
  }
  if (!in_range(config.timing.opening_ms, 3000, 180000) ||
      !in_range(config.timing.closing_ms, 3000, 180000)) {
    add_error(&errors, "timing.travelMs", "range",
              "Travel durations must be 3-180 seconds");
  }
  if (!in_range(config.timing.sensor_release_timeout_ms, 1000, 15000)) {
    add_error(&errors, "timing.sensorReleaseTimeoutMs", "range",
              "Sensor release timeout must be 1-15 seconds");
  }
  if (config.admin.salt.size() < 16 || config.admin.salt.size() > 32 ||
      config.admin.password_verifier.size() < 32 ||
      config.admin.password_verifier.size() > 64) {
    add_error(&errors, "admin.password", "missing",
              "Administrator password verifier is required");
  }
  if (config.admin.pbkdf2_iterations < 50000) {
    add_error(&errors, "admin.pbkdf2Iterations", "weak",
              "Password derivation work factor is too low");
  }
  return errors;
}

}  // namespace gate::config
