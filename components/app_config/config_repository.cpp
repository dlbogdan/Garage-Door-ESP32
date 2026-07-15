#include "config_repository.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "nvs.h"

namespace gate::config {
namespace {
constexpr char kNamespace[] = "gate_cfg";
constexpr char kBlobKey[] = "config_v4";
constexpr char kV3BlobKey[] = "config_v3";
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

struct PersistedDecoderInputV4 {
  std::uint8_t id;
  char label[33];
  PersistedInputV3 electrical;
};

struct PersistedPredicateV4 {
  std::uint8_t kind;
  std::uint8_t input_id;
  std::uint8_t stable_level;
  std::uint32_t stable_hold_ms;
  std::uint32_t minimum_interval_ms;
  std::uint32_t maximum_interval_ms;
  std::uint8_t minimum_edges;
  std::uint32_t observation_window_ms;
  std::uint32_t maximum_gap_ms;
};

struct PersistedPredicateGroupV4 {
  std::uint8_t predicate_count;
  PersistedPredicateV4
      predicates[gate::signal_decoder::DecoderLimits::kMaxPredicatesPerGroup];
};

struct PersistedRuleV4 {
  std::uint8_t id;
  char label[33];
  std::uint8_t enabled;
  std::uint8_t output_kind;
  std::uint8_t position;
  std::uint8_t movement;
  std::uint8_t fault;
  std::uint8_t group_count;
  PersistedPredicateGroupV4
      groups[gate::signal_decoder::DecoderLimits::kMaxGroupsPerRule];
  std::uint32_t entry_confirmation_ms;
  std::uint32_t loss_confirmation_ms;
  std::uint32_t match_age_limit_ms;
  std::uint8_t match_age_expiry;
};

struct PersistedDecoderV4 {
  std::uint8_t profile;
  std::uint8_t input_count;
  PersistedDecoderInputV4
      inputs[gate::signal_decoder::DecoderLimits::kMaxInputs];
  std::uint8_t rule_input_count;
  std::uint8_t rule_input_ids[gate::signal_decoder::DecoderLimits::kMaxInputs];
  std::uint8_t rule_count;
  PersistedRuleV4 rules[gate::signal_decoder::DecoderLimits::kMaxRules];
};

struct PersistedConfigV4 {
  PersistedConfigV3 v3;
  PersistedDecoderV4 decoder;
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

PersistedInputV3 encode_input(const FeedbackInputConfig& input) {
  return {static_cast<std::int8_t>(input.gpio),
          static_cast<std::uint8_t>(input.active_level),
          static_cast<std::uint8_t>(input.pull), input.debounce_ms};
}

FeedbackInputConfig decode_input(const PersistedInputV3& input) {
  return {input.gpio, static_cast<ActiveLevel>(input.active_level),
          static_cast<SensorPull>(input.pull), input.debounce_ms};
}

void encode_decoder(const FeedbackDecoderConfig& source,
                    PersistedDecoderV4* destination) {
  destination->profile = static_cast<std::uint8_t>(source.profile);
  destination->input_count = source.input_count;
  for (std::uint8_t i = 0; i < source.input_count; ++i) {
    destination->inputs[i].id = source.inputs[i].id;
    copy_string(destination->inputs[i].label, source.inputs[i].label);
    destination->inputs[i].electrical = encode_input(source.inputs[i].electrical);
  }
  destination->rule_input_count = source.rules.input_count;
  std::copy_n(source.rules.input_ids.begin(), source.rules.input_count,
              destination->rule_input_ids);
  destination->rule_count = source.rules.rule_count;
  for (std::uint8_t r = 0; r < source.rules.rule_count; ++r) {
    const auto& rule = source.rules.rules[r];
    auto& stored = destination->rules[r];
    stored.id = rule.id;
    copy_string(stored.label, source.rule_labels[r]);
    stored.enabled = rule.enabled;
    stored.output_kind = static_cast<std::uint8_t>(rule.output.kind);
    stored.position = static_cast<std::uint8_t>(rule.output.position);
    stored.movement = static_cast<std::uint8_t>(rule.output.movement);
    stored.fault = static_cast<std::uint8_t>(rule.output.fault);
    stored.group_count = rule.group_count;
    stored.entry_confirmation_ms = rule.movement.entry_confirmation_ms;
    stored.loss_confirmation_ms = rule.movement.loss_confirmation_ms;
    stored.match_age_limit_ms = rule.movement.match_age_limit_ms;
    stored.match_age_expiry = static_cast<std::uint8_t>(rule.movement.expiry);
    for (std::uint8_t g = 0; g < rule.group_count; ++g) {
      stored.groups[g].predicate_count = rule.groups[g].predicate_count;
      for (std::uint8_t p = 0; p < rule.groups[g].predicate_count; ++p) {
        const auto& predicate = rule.groups[g].predicates[p];
        auto& saved = stored.groups[g].predicates[p];
        saved.kind = static_cast<std::uint8_t>(predicate.kind);
        saved.input_id = predicate.input_id;
        saved.stable_level = predicate.stable.level;
        saved.stable_hold_ms = predicate.stable.hold_ms;
        saved.minimum_interval_ms = predicate.periodic.minimum_interval_ms;
        saved.maximum_interval_ms = predicate.periodic.maximum_interval_ms;
        saved.minimum_edges = predicate.periodic.minimum_edges;
        saved.observation_window_ms = predicate.periodic.observation_window_ms;
        saved.maximum_gap_ms = predicate.periodic.maximum_gap_ms;
      }
    }
  }
}

bool decode_decoder(const PersistedDecoderV4& source,
                    FeedbackDecoderConfig* destination) {
  if (source.input_count > gate::signal_decoder::DecoderLimits::kMaxInputs ||
      source.rule_input_count >
          gate::signal_decoder::DecoderLimits::kMaxInputs ||
      source.rule_count > gate::signal_decoder::DecoderLimits::kMaxRules) {
    return false;
  }
  // This bounded object is over 2 KiB. Repository decoding may run on startup
  // or an HTTP task, so do not place it on either constrained task stack.
  auto decoded = std::unique_ptr<FeedbackDecoderConfig>(
      new (std::nothrow) FeedbackDecoderConfig());
  if (decoded == nullptr) return false;
  decoded->profile = static_cast<FeedbackDecoderProfile>(source.profile);
  decoded->input_count = source.input_count;
  for (std::uint8_t i = 0; i < source.input_count; ++i) {
    decoded->inputs[i].id = source.inputs[i].id;
    decoded->inputs[i].label = read_string(source.inputs[i].label);
    decoded->inputs[i].electrical = decode_input(source.inputs[i].electrical);
  }
  decoded->rules.input_count = source.rule_input_count;
  std::copy_n(source.rule_input_ids, source.rule_input_count,
              decoded->rules.input_ids.begin());
  decoded->rules.rule_count = source.rule_count;
  for (std::uint8_t r = 0; r < source.rule_count; ++r) {
    const auto& stored = source.rules[r];
    if (stored.group_count >
        gate::signal_decoder::DecoderLimits::kMaxGroupsPerRule) return false;
    auto& rule = decoded->rules.rules[r];
    rule.id = stored.id;
    decoded->rule_labels[r] = read_string(stored.label);
    rule.enabled = stored.enabled != 0;
    rule.output.kind =
        static_cast<gate::signal_decoder::RuleOutputKind>(stored.output_kind);
    rule.output.position =
        static_cast<gate::signal_decoder::PositionValue>(stored.position);
    rule.output.movement =
        static_cast<gate::signal_decoder::MovementValue>(stored.movement);
    rule.output.fault =
        static_cast<gate::signal_decoder::FaultValue>(stored.fault);
    rule.group_count = stored.group_count;
    rule.movement = {
        stored.entry_confirmation_ms, stored.loss_confirmation_ms,
        stored.match_age_limit_ms,
        static_cast<gate::signal_decoder::MatchAgeExpiry>(
            stored.match_age_expiry)};
    for (std::uint8_t g = 0; g < stored.group_count; ++g) {
      if (stored.groups[g].predicate_count >
          gate::signal_decoder::DecoderLimits::kMaxPredicatesPerGroup) {
        return false;
      }
      rule.groups[g].predicate_count = stored.groups[g].predicate_count;
      for (std::uint8_t p = 0; p < stored.groups[g].predicate_count; ++p) {
        const auto& saved = stored.groups[g].predicates[p];
        auto& predicate = rule.groups[g].predicates[p];
        predicate.kind =
            static_cast<gate::signal_decoder::PredicateKind>(saved.kind);
        predicate.input_id = saved.input_id;
        predicate.stable = {saved.stable_level != 0, saved.stable_hold_ms};
        predicate.periodic = {
            saved.minimum_interval_ms, saved.maximum_interval_ms,
            saved.minimum_edges, saved.observation_window_ms,
            saved.maximum_gap_ms};
      }
    }
  }
  *destination = std::move(*decoded);
  return true;
}
}  // namespace

esp_err_t ConfigRepository::load(AppConfig* config) const {
  if (config == nullptr) return ESP_ERR_INVALID_ARG;
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (result != ESP_OK) return result;
  auto stored = std::unique_ptr<PersistedConfigV4>(
      new (std::nothrow) PersistedConfigV4());
  if (stored == nullptr) {
    nvs_close(handle);
    return ESP_ERR_NO_MEM;
  }
  std::size_t size = sizeof(*stored);
  result = nvs_get_blob(handle, kBlobKey, stored.get(), &size);
  bool migrated_legacy = false;
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    size = sizeof(stored->v3);
    result = nvs_get_blob(handle, kV3BlobKey, &stored->v3, &size);
    if (result == ESP_OK && size == sizeof(PersistedConfigV3) &&
        stored->v3.common_and_slot_a.v1.magic == kMagic &&
        stored->v3.common_and_slot_a.v1.schema_version == 3) {
      stored->v3.common_and_slot_a.v1.schema_version = kSchemaVersion;
      stored->decoder.profile =
          static_cast<std::uint8_t>(FeedbackDecoderProfile::kEndpointPreset);
      migrated_legacy = true;
      size = sizeof(PersistedConfigV4);
    } else if (result == ESP_ERR_NVS_NOT_FOUND) {
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
        stored->v3.common_and_slot_a = legacy_stored;
        stored->v3.common_and_slot_a.v1.schema_version = kSchemaVersion;
        if (legacy_version == 1) {
          stored->v3.common_and_slot_a.sensor_active_endpoint =
              static_cast<std::uint8_t>(FeedbackEndpoint::kClosed);
          stored->v3.common_and_slot_a.sensor_endpoint_stability_ms = 2000;
        }
        stored->v3.operator_profile =
            static_cast<std::uint8_t>(OperatorProfile::kSequential);
        stored->v3.feedback_topology =
            static_cast<std::uint8_t>(FeedbackTopology::kSingle);
        stored->decoder.profile =
            static_cast<std::uint8_t>(FeedbackDecoderProfile::kEndpointPreset);
        migrated_legacy = true;
        size = sizeof(PersistedConfigV4);
      }
    }
  }
  nvs_close(handle);
  if (result != ESP_OK) return result;
  if (size != sizeof(PersistedConfigV4) ||
      stored->v3.common_and_slot_a.v1.magic != kMagic ||
      stored->v3.common_and_slot_a.v1.schema_version != kSchemaVersion) {
    return ESP_ERR_INVALID_VERSION;
  }

  const PersistedConfigV1& legacy = stored->v3.common_and_slot_a.v1;

  auto loaded = std::unique_ptr<AppConfig>(new (std::nothrow) AppConfig());
  if (loaded == nullptr) return ESP_ERR_NO_MEM;
  loaded->schema_version = kSchemaVersion;
  loaded->wifi = {read_string(legacy.wifi_ssid),
                 read_string(legacy.wifi_password), legacy.connection_deadline_ms};
  loaded->access_point = {read_string(legacy.ap_password), legacy.ap_shutdown_ms};
  loaded->homekit = {read_string(legacy.display_name),
                    read_string(legacy.setup_code), read_string(legacy.setup_id)};
  loaded->gate_operator.profile =
      static_cast<OperatorProfile>(stored->v3.operator_profile);
  loaded->gate_operator.feedback_topology =
      static_cast<FeedbackTopology>(stored->v3.feedback_topology);
  loaded->gate_operator.minimum_interval_ms = legacy.relay_minimum_interval_ms;
  auto& first_output = loaded->gate_operator.profile == OperatorProfile::kSequential
                           ? loaded->gate_operator.step
                           : loaded->gate_operator.open;
  first_output = {legacy.relay_gpio,
                  static_cast<ActiveLevel>(legacy.relay_active_level),
                  legacy.relay_pulse_ms};
  loaded->gate_operator.close = {
       stored->v3.second_output.gpio,
       static_cast<ActiveLevel>(stored->v3.second_output.active_level),
       stored->v3.second_output.pulse_ms};
  loaded->gate_operator.single_active_endpoint = static_cast<FeedbackEndpoint>(
       stored->v3.common_and_slot_a.sensor_active_endpoint);
  loaded->gate_operator.endpoint_stability_ms =
       stored->v3.common_and_slot_a.sensor_endpoint_stability_ms;
  auto& first_input =
      loaded->gate_operator.feedback_topology == FeedbackTopology::kSingle
          ? loaded->gate_operator.single_feedback
          : loaded->gate_operator.opened_feedback;
  first_input = {legacy.sensor_gpio,
                 static_cast<ActiveLevel>(legacy.sensor_active_level),
                 static_cast<SensorPull>(legacy.sensor_pull),
                 legacy.sensor_debounce_ms};
  loaded->gate_operator.closed_feedback = {
       stored->v3.second_feedback.gpio,
       static_cast<ActiveLevel>(stored->v3.second_feedback.active_level),
       static_cast<SensorPull>(stored->v3.second_feedback.pull),
       stored->v3.second_feedback.debounce_ms};
  if (!decode_decoder(stored->decoder, &loaded->gate_operator.decoder)) {
    return ESP_ERR_INVALID_SIZE;
  }
  loaded->timing = {legacy.opening_ms, legacy.closing_ms,
                   legacy.sensor_release_timeout_ms};
  if (legacy.admin_salt_length > sizeof(legacy.admin_salt) ||
      legacy.admin_verifier_length > sizeof(legacy.admin_verifier)) {
    return ESP_ERR_INVALID_SIZE;
  }
  loaded->admin.salt.assign(legacy.admin_salt,
                           legacy.admin_salt + legacy.admin_salt_length);
  loaded->admin.password_verifier.assign(
      legacy.admin_verifier,
      legacy.admin_verifier + legacy.admin_verifier_length);
  loaded->admin.pbkdf2_iterations = legacy.pbkdf2_iterations;
  if (!validate(*loaded).empty()) return ESP_ERR_INVALID_STATE;
  if (migrated_legacy) {
    result = save(*loaded);
    if (result != ESP_OK) return result;
  }
  *config = std::move(*loaded);
  return ESP_OK;
}

esp_err_t ConfigRepository::save(const AppConfig& config) const {
  if (!validate(config).empty()) return ESP_ERR_INVALID_ARG;
  auto stored = std::unique_ptr<PersistedConfigV4>(
      new (std::nothrow) PersistedConfigV4());
  if (stored == nullptr) return ESP_ERR_NO_MEM;
  PersistedConfigV1& legacy = stored->v3.common_and_slot_a.v1;
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
  stored->v3.operator_profile = static_cast<std::uint8_t>(config.gate_operator.profile);
  stored->v3.feedback_topology =
      static_cast<std::uint8_t>(config.gate_operator.feedback_topology);
  const auto& first_output =
      config.gate_operator.profile == OperatorProfile::kSequential
          ? config.gate_operator.step
          : config.gate_operator.open;
  legacy.relay_gpio = first_output.gpio;
  legacy.relay_active_level = static_cast<std::uint8_t>(first_output.active_level);
  legacy.relay_pulse_ms = first_output.pulse_ms;
  legacy.relay_minimum_interval_ms = config.gate_operator.minimum_interval_ms;
  stored->v3.second_output = {static_cast<std::int8_t>(config.gate_operator.close.gpio),
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
  stored->v3.common_and_slot_a.sensor_active_endpoint =
      static_cast<std::uint8_t>(config.gate_operator.single_active_endpoint);
  stored->v3.common_and_slot_a.sensor_endpoint_stability_ms =
      config.gate_operator.endpoint_stability_ms;
  stored->v3.second_feedback = {
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
  encode_decoder(config.gate_operator.decoder, &stored->decoder);

  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, kBlobKey, stored.get(), sizeof(*stored));
  }
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
