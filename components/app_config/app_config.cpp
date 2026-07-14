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

bool has_step_output(const OperatorConfig& config) {
  return config.profile == OperatorProfile::kSequential;
}

bool has_open_output(const OperatorConfig& config) {
  return config.profile == OperatorProfile::kDirectional;
}

bool has_close_output(const OperatorConfig& config) {
  return config.profile == OperatorProfile::kDirectional;
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
  const auto& operator_config = config.gate_operator;
  const PulseOutputConfig* outputs[2]{};
  const char* output_fields[2]{};
  std::size_t output_count = 0;
  if (operator_config.profile == OperatorProfile::kSequential) {
    outputs[output_count] = &operator_config.step;
    output_fields[output_count++] = "operator.actuators.step";
  } else if (operator_config.profile == OperatorProfile::kDirectional) {
    outputs[output_count] = &operator_config.open;
    output_fields[output_count++] = "operator.actuators.open";
    outputs[output_count] = &operator_config.close;
    output_fields[output_count++] = "operator.actuators.close";
  } else {
    add_error(&errors, "operator.profile", "unsupported",
              "Unsupported operator profile");
  }

  const FeedbackInputConfig* inputs[2]{};
  const char* input_fields[2]{};
  std::size_t input_count = 0;
  if (operator_config.feedback_topology == FeedbackTopology::kSingle) {
    inputs[input_count] = &operator_config.single_feedback;
    input_fields[input_count++] = "operator.feedback.single";
  } else if (operator_config.feedback_topology == FeedbackTopology::kDual) {
    inputs[input_count] = &operator_config.opened_feedback;
    input_fields[input_count++] = "operator.feedback.opened";
    inputs[input_count] = &operator_config.closed_feedback;
    input_fields[input_count++] = "operator.feedback.closed";
  } else {
    add_error(&errors, "operator.feedback.mode", "unsupported",
              "Unsupported feedback topology");
  }

  for (std::size_t index = 0; index < output_count; ++index) {
    const auto& output = *outputs[index];
    const std::string gpio_field = std::string(output_fields[index]) + ".gpio";
    if (!is_safe_gpio(output.gpio, true)) {
      errors.push_back({gpio_field, "unsafe", "Unsafe actuator GPIO"});
    }
    if (!in_range(output.pulse_ms, 100, 2000)) {
      errors.push_back({std::string(output_fields[index]) + ".pulseMs", "range",
                        "Pulse must be 100-2000 ms"});
    }
  }
  if (!in_range(operator_config.minimum_interval_ms, 500, 10000)) {
    add_error(&errors, "operator.minimumIntervalMs", "range",
              "Minimum interval must be 500-10000 ms");
  }
  if (!in_range(operator_config.endpoint_stability_ms, 1000, 10000)) {
    add_error(&errors, "operator.feedback.stabilityMs", "range",
              "Endpoint stability must be 1-10 seconds");
  }
  for (std::size_t index = 0; index < input_count; ++index) {
    const auto& input = *inputs[index];
    const std::string base = input_fields[index];
    if (!is_safe_gpio(input.gpio, false)) {
      errors.push_back({base + ".gpio", "unsafe", "Unsafe feedback GPIO"});
    }
    if (!in_range(input.debounce_ms, 10, 500)) {
      errors.push_back({base + ".debounceMs", "range",
                        "Debounce must be 10-500 ms"});
    }
    if (input.gpio >= 34 && input.pull != SensorPull::kNone) {
      errors.push_back({base + ".pull", "unsupported",
                        "GPIO34-39 do not support internal pulls"});
    }
  }
  for (std::size_t left = 0; left < output_count; ++left) {
    for (std::size_t right = left + 1; right < output_count; ++right) {
      if (outputs[left]->gpio >= 0 && outputs[left]->gpio == outputs[right]->gpio) {
        errors.push_back({std::string(output_fields[right]) + ".gpio", "collision",
                          "Actuator GPIOs must differ"});
      }
    }
    for (std::size_t input = 0; input < input_count; ++input) {
      if (outputs[left]->gpio >= 0 && outputs[left]->gpio == inputs[input]->gpio) {
        errors.push_back({std::string(input_fields[input]) + ".gpio", "collision",
                          "Actuator and feedback GPIOs must differ"});
      }
    }
  }
  if (input_count == 2 && inputs[0]->gpio >= 0 &&
      inputs[0]->gpio == inputs[1]->gpio) {
    errors.push_back({std::string(input_fields[1]) + ".gpio", "collision",
                      "Feedback GPIOs must differ"});
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
