#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gate::config {

inline constexpr std::uint32_t kSchemaVersion = 1;

enum class ActiveLevel : std::uint8_t { kLow = 0, kHigh = 1 };
enum class SensorPull : std::uint8_t { kNone = 0, kUp = 1, kDown = 2 };

struct WifiConfig {
  std::string ssid;
  std::string password;
  std::uint32_t connection_deadline_ms{45000};
};

struct AccessPointConfig {
  std::string password;
  std::uint32_t stable_station_shutdown_ms{60000};
};

struct HomeKitConfig {
  std::string display_name{"Gate"};
  std::string setup_code;
  std::string setup_id;
};

struct RelayConfig {
  int gpio{-1};
  ActiveLevel active_level{ActiveLevel::kLow};
  std::uint32_t pulse_ms{500};
  std::uint32_t minimum_interval_ms{1500};
};

struct SensorConfig {
  int gpio{-1};
  ActiveLevel active_level{ActiveLevel::kLow};
  SensorPull pull{SensorPull::kUp};
  std::uint32_t debounce_ms{50};
};

struct TimingConfig {
  std::uint32_t opening_ms{20000};
  std::uint32_t closing_ms{20000};
  std::uint32_t sensor_release_timeout_ms{3000};
};

struct AdminConfig {
  std::vector<std::uint8_t> salt;
  std::vector<std::uint8_t> password_verifier;
  std::uint32_t pbkdf2_iterations{120000};
};

struct AppConfig {
  std::uint32_t schema_version{kSchemaVersion};
  WifiConfig wifi;
  AccessPointConfig access_point;
  HomeKitConfig homekit;
  RelayConfig relay;
  SensorConfig sensor;
  TimingConfig timing;
  AdminConfig admin;
};

struct ValidationError {
  std::string field;
  std::string code;
  std::string message;
};

std::vector<ValidationError> validate(const AppConfig& config);
bool is_provisioned(const AppConfig& config);
bool is_safe_gpio(int gpio, bool output);
bool is_valid_homekit_setup_code(const std::string& code);
bool is_valid_homekit_setup_id(const std::string& setup_id);

}  // namespace gate::config
