#include "config_repository.hpp"

#include <cstring>
#include <vector>

#include "nvs.h"

namespace gate::config {
namespace {
constexpr char kNamespace[] = "gate_cfg";
constexpr char kBlobKey[] = "config_v3";
constexpr char kLegacyBlobKey[] = "config_v1";
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

struct PersistedConfigV2 {
  PersistedConfigV1 v1;
  std::uint8_t sensor_active_endpoint;
  std::uint32_t sensor_endpoint_stability_ms;
};

struct PersistedOutputV3 {
  std::int8_t gpio;
  std::uint8_t active_level;
  std::uint32_t pulse_ms;
};

struct PersistedInputV3 {
  std::int8_t gpio;
  std::uint8_t active_level;
  std::uint8_t pull;
  std::uint32_t debounce_ms;
};

struct PersistedConfigV3 {
  PersistedConfigV2 common_and_slot_a;
  std::uint8_t operator_profile;
  std::uint8_t feedback_topology;
  PersistedOutputV3 second_output;
  PersistedInputV3 second_feedback;
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
  PersistedConfigV3 stored{};
  std::size_t size = sizeof(stored);
  result = nvs_get_blob(handle, kBlobKey, &stored, &size);
  bool migrated_legacy = false;
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    PersistedConfigV2 legacy_stored{};
    size = sizeof(legacy_stored);
    result = nvs_get_blob(handle, kLegacyBlobKey, &legacy_stored, &size);
    if (result == ESP_OK &&
        (size == sizeof(PersistedConfigV1) ||
         size == sizeof(PersistedConfigV2)) &&
        legacy_stored.v1.magic == kMagic &&
        (legacy_stored.v1.schema_version == 1 ||
         legacy_stored.v1.schema_version == 2)) {
      const std::uint32_t legacy_version = legacy_stored.v1.schema_version;
      stored.common_and_slot_a = legacy_stored;
      stored.common_and_slot_a.v1.schema_version = kSchemaVersion;
      if (legacy_version == 1) {
        stored.common_and_slot_a.sensor_active_endpoint =
            static_cast<std::uint8_t>(FeedbackEndpoint::kClosed);
        stored.common_and_slot_a.sensor_endpoint_stability_ms = 2000;
      }
      stored.operator_profile =
          static_cast<std::uint8_t>(OperatorProfile::kSequential);
      stored.feedback_topology =
          static_cast<std::uint8_t>(FeedbackTopology::kSingle);
      migrated_legacy = true;
      size = sizeof(PersistedConfigV3);
    }
  }
  nvs_close(handle);
  if (result != ESP_OK) return result;
  if (size != sizeof(PersistedConfigV3) ||
      stored.common_and_slot_a.v1.magic != kMagic ||
      stored.common_and_slot_a.v1.schema_version != kSchemaVersion) {
    return ESP_ERR_INVALID_VERSION;
  }

  const PersistedConfigV1& legacy = stored.common_and_slot_a.v1;

  AppConfig loaded;
  loaded.schema_version = kSchemaVersion;
  loaded.wifi = {read_string(legacy.wifi_ssid),
                 read_string(legacy.wifi_password), legacy.connection_deadline_ms};
  loaded.access_point = {read_string(legacy.ap_password), legacy.ap_shutdown_ms};
  loaded.homekit = {read_string(legacy.display_name),
                    read_string(legacy.setup_code), read_string(legacy.setup_id)};
  loaded.gate_operator.profile =
      static_cast<OperatorProfile>(stored.operator_profile);
  loaded.gate_operator.feedback_topology =
      static_cast<FeedbackTopology>(stored.feedback_topology);
  loaded.gate_operator.minimum_interval_ms = legacy.relay_minimum_interval_ms;
  auto& first_output = loaded.gate_operator.profile == OperatorProfile::kSequential
                           ? loaded.gate_operator.step
                           : loaded.gate_operator.open;
  first_output = {legacy.relay_gpio,
                  static_cast<ActiveLevel>(legacy.relay_active_level),
                  legacy.relay_pulse_ms};
  loaded.gate_operator.close = {
      stored.second_output.gpio,
      static_cast<ActiveLevel>(stored.second_output.active_level),
      stored.second_output.pulse_ms};
  loaded.gate_operator.single_active_endpoint = static_cast<FeedbackEndpoint>(
      stored.common_and_slot_a.sensor_active_endpoint);
  loaded.gate_operator.endpoint_stability_ms =
      stored.common_and_slot_a.sensor_endpoint_stability_ms;
  auto& first_input =
      loaded.gate_operator.feedback_topology == FeedbackTopology::kSingle
          ? loaded.gate_operator.single_feedback
          : loaded.gate_operator.opened_feedback;
  first_input = {legacy.sensor_gpio,
                 static_cast<ActiveLevel>(legacy.sensor_active_level),
                 static_cast<SensorPull>(legacy.sensor_pull),
                 legacy.sensor_debounce_ms};
  loaded.gate_operator.closed_feedback = {
      stored.second_feedback.gpio,
      static_cast<ActiveLevel>(stored.second_feedback.active_level),
      static_cast<SensorPull>(stored.second_feedback.pull),
      stored.second_feedback.debounce_ms};
  loaded.timing = {legacy.opening_ms, legacy.closing_ms,
                   legacy.sensor_release_timeout_ms};
  if (legacy.admin_salt_length > sizeof(legacy.admin_salt) ||
      legacy.admin_verifier_length > sizeof(legacy.admin_verifier)) {
    return ESP_ERR_INVALID_SIZE;
  }
  loaded.admin.salt.assign(legacy.admin_salt,
                           legacy.admin_salt + legacy.admin_salt_length);
  loaded.admin.password_verifier.assign(
      legacy.admin_verifier,
      legacy.admin_verifier + legacy.admin_verifier_length);
  loaded.admin.pbkdf2_iterations = legacy.pbkdf2_iterations;
  if (!validate(loaded).empty()) return ESP_ERR_INVALID_STATE;
  if (migrated_legacy) {
    result = save(loaded);
    if (result != ESP_OK) return result;
  }
  *config = std::move(loaded);
  return ESP_OK;
}

esp_err_t ConfigRepository::save(const AppConfig& config) const {
  if (!validate(config).empty()) return ESP_ERR_INVALID_ARG;
  PersistedConfigV3 stored{};
  PersistedConfigV1& legacy = stored.common_and_slot_a.v1;
  legacy.magic = kMagic;
  legacy.schema_version = kSchemaVersion;
  copy_string(legacy.wifi_ssid, config.wifi.ssid);
  copy_string(legacy.wifi_password, config.wifi.password);
  legacy.connection_deadline_ms = config.wifi.connection_deadline_ms;
  copy_string(legacy.ap_password, config.access_point.password);
  legacy.ap_shutdown_ms = config.access_point.stable_station_shutdown_ms;
  copy_string(legacy.display_name, config.homekit.display_name);
  copy_string(legacy.setup_code, config.homekit.setup_code);
  copy_string(legacy.setup_id, config.homekit.setup_id);
  stored.operator_profile = static_cast<std::uint8_t>(config.gate_operator.profile);
  stored.feedback_topology =
      static_cast<std::uint8_t>(config.gate_operator.feedback_topology);
  const auto& first_output =
      config.gate_operator.profile == OperatorProfile::kSequential
          ? config.gate_operator.step
          : config.gate_operator.open;
  legacy.relay_gpio = first_output.gpio;
  legacy.relay_active_level = static_cast<std::uint8_t>(first_output.active_level);
  legacy.relay_pulse_ms = first_output.pulse_ms;
  legacy.relay_minimum_interval_ms = config.gate_operator.minimum_interval_ms;
  stored.second_output = {static_cast<std::int8_t>(config.gate_operator.close.gpio),
                          static_cast<std::uint8_t>(config.gate_operator.close.active_level),
                          config.gate_operator.close.pulse_ms};
  const auto& first_input =
      config.gate_operator.feedback_topology == FeedbackTopology::kSingle
          ? config.gate_operator.single_feedback
          : config.gate_operator.opened_feedback;
  legacy.sensor_gpio = first_input.gpio;
  legacy.sensor_active_level = static_cast<std::uint8_t>(first_input.active_level);
  legacy.sensor_pull = static_cast<std::uint8_t>(first_input.pull);
  legacy.sensor_debounce_ms = first_input.debounce_ms;
  stored.common_and_slot_a.sensor_active_endpoint =
      static_cast<std::uint8_t>(config.gate_operator.single_active_endpoint);
  stored.common_and_slot_a.sensor_endpoint_stability_ms =
      config.gate_operator.endpoint_stability_ms;
  stored.second_feedback = {
      static_cast<std::int8_t>(config.gate_operator.closed_feedback.gpio),
      static_cast<std::uint8_t>(config.gate_operator.closed_feedback.active_level),
      static_cast<std::uint8_t>(config.gate_operator.closed_feedback.pull),
      config.gate_operator.closed_feedback.debounce_ms};
  legacy.opening_ms = config.timing.opening_ms;
  legacy.closing_ms = config.timing.closing_ms;
  legacy.sensor_release_timeout_ms = config.timing.sensor_release_timeout_ms;
  legacy.admin_salt_length = config.admin.salt.size();
  legacy.admin_verifier_length = config.admin.password_verifier.size();
  std::memcpy(legacy.admin_salt, config.admin.salt.data(),
              config.admin.salt.size());
  std::memcpy(legacy.admin_verifier, config.admin.password_verifier.data(),
              config.admin.password_verifier.size());
  legacy.pbkdf2_iterations = config.admin.pbkdf2_iterations;

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
