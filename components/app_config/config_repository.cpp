#include "config_repository.hpp"

#include <cstring>
#include <vector>

#include "nvs.h"

namespace gate::config {
namespace {
constexpr char kNamespace[] = "gate_cfg";
constexpr char kBlobKey[] = "config_v1";
constexpr std::uint32_t kMagic = 0x47415445;

#pragma pack(push, 1)
struct PersistedConfigV1 {
  std::uint32_t magic;
  std::uint32_t schema_version;
  char wifi_ssid[33];
  char wifi_password[64];
  std::uint32_t connection_deadline_ms;
  char ap_password[64];
  std::uint32_t ap_shutdown_ms;
  char display_name[65];
  char setup_code[9];
  char setup_id[5];
  std::int8_t relay_gpio;
  std::uint8_t relay_active_level;
  std::uint32_t relay_pulse_ms;
  std::uint32_t relay_minimum_interval_ms;
  std::int8_t sensor_gpio;
  std::uint8_t sensor_active_level;
  std::uint8_t sensor_pull;
  std::uint32_t sensor_debounce_ms;
  std::uint32_t opening_ms;
  std::uint32_t closing_ms;
  std::uint32_t sensor_release_timeout_ms;
  std::uint8_t admin_salt[32];
  std::uint8_t admin_salt_length;
  std::uint8_t admin_verifier[64];
  std::uint8_t admin_verifier_length;
  std::uint32_t pbkdf2_iterations;
};
#pragma pack(pop)

template <std::size_t Size>
void copy_string(char (&destination)[Size], const std::string& source) {
  std::memset(destination, 0, Size);
  std::memcpy(destination, source.data(), std::min(source.size(), Size - 1));
}

template <std::size_t Size>
std::string read_string(const char (&source)[Size]) {
  return std::string(source, strnlen(source, Size));
}
}  // namespace

esp_err_t ConfigRepository::load(AppConfig* config) const {
  if (config == nullptr) return ESP_ERR_INVALID_ARG;
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (result != ESP_OK) return result;
  PersistedConfigV1 stored{};
  std::size_t size = sizeof(stored);
  result = nvs_get_blob(handle, kBlobKey, &stored, &size);
  nvs_close(handle);
  if (result != ESP_OK) return result;
  if (size != sizeof(stored) || stored.magic != kMagic ||
      stored.schema_version != kSchemaVersion) return ESP_ERR_INVALID_VERSION;

  AppConfig loaded;
  loaded.wifi = {read_string(stored.wifi_ssid),
                 read_string(stored.wifi_password), stored.connection_deadline_ms};
  loaded.access_point = {read_string(stored.ap_password), stored.ap_shutdown_ms};
  loaded.homekit = {read_string(stored.display_name),
                    read_string(stored.setup_code), read_string(stored.setup_id)};
  loaded.relay = {stored.relay_gpio,
                  static_cast<ActiveLevel>(stored.relay_active_level),
                  stored.relay_pulse_ms, stored.relay_minimum_interval_ms};
  loaded.sensor = {stored.sensor_gpio,
                   static_cast<ActiveLevel>(stored.sensor_active_level),
                   static_cast<SensorPull>(stored.sensor_pull),
                   stored.sensor_debounce_ms};
  loaded.timing = {stored.opening_ms, stored.closing_ms,
                   stored.sensor_release_timeout_ms};
  if (stored.admin_salt_length > sizeof(stored.admin_salt) ||
      stored.admin_verifier_length > sizeof(stored.admin_verifier)) {
    return ESP_ERR_INVALID_SIZE;
  }
  loaded.admin.salt.assign(stored.admin_salt,
                           stored.admin_salt + stored.admin_salt_length);
  loaded.admin.password_verifier.assign(
      stored.admin_verifier,
      stored.admin_verifier + stored.admin_verifier_length);
  loaded.admin.pbkdf2_iterations = stored.pbkdf2_iterations;
  if (!validate(loaded).empty()) return ESP_ERR_INVALID_STATE;
  *config = std::move(loaded);
  return ESP_OK;
}

esp_err_t ConfigRepository::save(const AppConfig& config) const {
  if (!validate(config).empty()) return ESP_ERR_INVALID_ARG;
  PersistedConfigV1 stored{};
  stored.magic = kMagic;
  stored.schema_version = kSchemaVersion;
  copy_string(stored.wifi_ssid, config.wifi.ssid);
  copy_string(stored.wifi_password, config.wifi.password);
  stored.connection_deadline_ms = config.wifi.connection_deadline_ms;
  copy_string(stored.ap_password, config.access_point.password);
  stored.ap_shutdown_ms = config.access_point.stable_station_shutdown_ms;
  copy_string(stored.display_name, config.homekit.display_name);
  copy_string(stored.setup_code, config.homekit.setup_code);
  copy_string(stored.setup_id, config.homekit.setup_id);
  stored.relay_gpio = config.relay.gpio;
  stored.relay_active_level = static_cast<std::uint8_t>(config.relay.active_level);
  stored.relay_pulse_ms = config.relay.pulse_ms;
  stored.relay_minimum_interval_ms = config.relay.minimum_interval_ms;
  stored.sensor_gpio = config.sensor.gpio;
  stored.sensor_active_level = static_cast<std::uint8_t>(config.sensor.active_level);
  stored.sensor_pull = static_cast<std::uint8_t>(config.sensor.pull);
  stored.sensor_debounce_ms = config.sensor.debounce_ms;
  stored.opening_ms = config.timing.opening_ms;
  stored.closing_ms = config.timing.closing_ms;
  stored.sensor_release_timeout_ms = config.timing.sensor_release_timeout_ms;
  stored.admin_salt_length = config.admin.salt.size();
  stored.admin_verifier_length = config.admin.password_verifier.size();
  std::memcpy(stored.admin_salt, config.admin.salt.data(),
              config.admin.salt.size());
  std::memcpy(stored.admin_verifier, config.admin.password_verifier.data(),
              config.admin.password_verifier.size());
  stored.pbkdf2_iterations = config.admin.pbkdf2_iterations;

  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result == ESP_OK) result = nvs_set_blob(handle, kBlobKey, &stored, sizeof(stored));
  if (result == ESP_OK) result = nvs_commit(handle);
  if (handle) nvs_close(handle);
  return result;
}

esp_err_t ConfigRepository::erase() const {
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) return result;
  result = nvs_erase_all(handle);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result;
}

}  // namespace gate::config
