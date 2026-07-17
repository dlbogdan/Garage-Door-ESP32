#include "gate_profile.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

#include "cJSON.h"
#include "mbedtls/sha256.h"

namespace gate::profile {
namespace {

using gate::config::ActiveLevel;
using gate::config::FeedbackDecoderProfile;
using gate::config::FeedbackEndpoint;
using gate::config::FeedbackInputConfig;
using gate::config::FeedbackTopology;
using gate::config::OperatorProfile;
using gate::config::PulseOutputConfig;
using gate::config::SensorPull;
using namespace gate::signal_decoder;

struct Json final {
  cJSON* value{nullptr};
  ~Json() { cJSON_Delete(value); }
};

const char* level_name(ActiveLevel value) {
  return value == ActiveLevel::kHigh ? "high" : "low";
}

const char* pull_name(SensorPull value) {
  if (value == SensorPull::kNone) return "none";
  if (value == SensorPull::kDown) return "down";
  return "up";
}

const char* expiry_name(MatchAgeExpiry value) {
  if (value == MatchAgeExpiry::kUnknown) return "unknown";
  if (value == MatchAgeExpiry::kObstructed) return "obstructed";
  if (value == MatchAgeExpiry::kUnknownAndObstructed) {
    return "unknownAndObstructed";
  }
  return "none";
}

std::string output_name(const RuleOutputConfig& output) {
  if (output.kind == RuleOutputKind::kPosition) {
    return output.position == PositionValue::kOpened ? "opened" : "closed";
  }
  if (output.kind == RuleOutputKind::kMovement) {
    if (output.movement == MovementValue::kOpening) return "opening";
    if (output.movement == MovementValue::kClosing) return "closing";
    return "stopped";
  }
  return "obstructed";
}

cJSON* output_json(const PulseOutputConfig& output) {
  cJSON* value = cJSON_CreateObject();
  cJSON_AddNumberToObject(value, "gpio", output.gpio);
  cJSON_AddStringToObject(value, "activeLevel", level_name(output.active_level));
  cJSON_AddNumberToObject(value, "pulseMs", output.pulse_ms);
  return value;
}

cJSON* input_json(const FeedbackInputConfig& input) {
  cJSON* value = cJSON_CreateObject();
  cJSON_AddNumberToObject(value, "gpio", input.gpio);
  cJSON_AddStringToObject(value, "activeLevel", level_name(input.active_level));
  cJSON_AddStringToObject(value, "pull", pull_name(input.pull));
  cJSON_AddNumberToObject(value, "debounceMs", input.debounce_ms);
  return value;
}

cJSON* decoder_json(const gate::config::FeedbackDecoderConfig& decoder) {
  cJSON* value = cJSON_CreateObject();
  cJSON_AddStringToObject(
      value, "profile",
      decoder.profile == FeedbackDecoderProfile::kCustomRules
          ? "customRules"
          : "endpointPreset");
  cJSON* inputs = cJSON_AddArrayToObject(value, "inputs");
  for (std::uint8_t i = 0; i < decoder.input_count; ++i) {
    const auto& input = decoder.inputs[i];
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "id", input.id);
    cJSON_AddStringToObject(item, "label", input.label.c_str());
    cJSON_AddItemToObject(item, "electrical", input_json(input.electrical));
    cJSON_AddItemToArray(inputs, item);
  }
  cJSON* rules = cJSON_AddArrayToObject(value, "rules");
  for (std::uint8_t r = 0; r < decoder.rules.rule_count; ++r) {
    const auto& rule = decoder.rules.rules[r];
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "id", rule.id);
    cJSON_AddStringToObject(item, "label", decoder.rule_labels[r].c_str());
    cJSON_AddBoolToObject(item, "enabled", rule.enabled);
    cJSON_AddStringToObject(item, "outcome", output_name(rule.output).c_str());
    cJSON* lifecycle = cJSON_AddObjectToObject(item, "lifecycle");
    cJSON_AddNumberToObject(lifecycle, "entryConfirmationMs",
                            rule.movement.entry_confirmation_ms);
    cJSON_AddNumberToObject(lifecycle, "lossConfirmationMs",
                            rule.movement.loss_confirmation_ms);
    cJSON_AddNumberToObject(lifecycle, "matchAgeLimitMs",
                            rule.movement.match_age_limit_ms);
    cJSON_AddStringToObject(lifecycle, "matchAgeExpiry",
                            expiry_name(rule.movement.expiry));
    cJSON* groups = cJSON_AddArrayToObject(item, "groups");
    for (std::uint8_t g = 0; g < rule.group_count; ++g) {
      cJSON* group = cJSON_CreateArray();
      for (std::uint8_t p = 0; p < rule.groups[g].predicate_count; ++p) {
        const auto& predicate = rule.groups[g].predicates[p];
        cJSON* predicate_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(predicate_json, "inputId", predicate.input_id);
        if (predicate.kind == PredicateKind::kStableLevel) {
          cJSON_AddStringToObject(predicate_json, "kind", "stableLevel");
          cJSON_AddNumberToObject(predicate_json, "level",
                                  predicate.stable.level ? 1 : 0);
          cJSON_AddNumberToObject(predicate_json, "holdMs",
                                  predicate.stable.hold_ms);
        } else {
          cJSON_AddStringToObject(predicate_json, "kind", "periodicEdges");
          cJSON_AddNumberToObject(predicate_json, "minimumIntervalMs",
                                  predicate.periodic.minimum_interval_ms);
          cJSON_AddNumberToObject(predicate_json, "maximumIntervalMs",
                                  predicate.periodic.maximum_interval_ms);
          cJSON_AddNumberToObject(predicate_json, "minimumEdges",
                                  predicate.periodic.minimum_edges);
          cJSON_AddNumberToObject(predicate_json, "observationWindowMs",
                                  predicate.periodic.observation_window_ms);
          cJSON_AddNumberToObject(predicate_json, "maximumGapMs",
                                  predicate.periodic.maximum_gap_ms);
        }
        cJSON_AddItemToArray(group, predicate_json);
      }
      cJSON_AddItemToArray(groups, group);
    }
    cJSON_AddItemToArray(rules, item);
  }
  return value;
}

cJSON* build_json(const gate::config::AppConfig& config,
                  const Metadata& metadata) {
  const auto& op = config.gate_operator;
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "format", kFormat);
  cJSON_AddNumberToObject(root, "version", kVersion);
  cJSON_AddStringToObject(root, "project", kProject);
  cJSON* target = cJSON_AddObjectToObject(root, "target");
  cJSON_AddStringToObject(target, "vendor", metadata.vendor.c_str());
  cJSON_AddStringToObject(target, "model", metadata.model.c_str());
  cJSON_AddStringToObject(target, "name", metadata.name.c_str());
  cJSON_AddStringToObject(target, "notes", metadata.notes.c_str());
  cJSON* compatibility = cJSON_AddObjectToObject(root, "compatibility");
  cJSON_AddNumberToObject(compatibility, "appSchemaVersion",
                          gate::config::kSchemaVersion);
  cJSON* operator_json = cJSON_AddObjectToObject(root, "operator");
  cJSON_AddStringToObject(operator_json, "profile",
                          op.profile == OperatorProfile::kDirectional
                              ? "directional"
                              : "sequential");
  cJSON_AddNumberToObject(operator_json, "minimumIntervalMs",
                          op.minimum_interval_ms);
  cJSON* outputs = cJSON_AddObjectToObject(operator_json, "outputs");
  cJSON_AddItemToObject(outputs, "step", output_json(op.step));
  cJSON_AddItemToObject(outputs, "open", output_json(op.open));
  cJSON_AddItemToObject(outputs, "close", output_json(op.close));
  cJSON* feedback = cJSON_AddObjectToObject(operator_json, "feedback");
  cJSON_AddStringToObject(feedback, "topology",
                          op.feedback_topology == FeedbackTopology::kDual
                              ? "dual"
                              : "single");
  cJSON_AddStringToObject(feedback, "singleActiveEndpoint",
                          op.single_active_endpoint == FeedbackEndpoint::kOpen
                              ? "open"
                              : "closed");
  cJSON_AddNumberToObject(feedback, "endpointStabilityMs",
                          op.endpoint_stability_ms);
  cJSON_AddItemToObject(feedback, "single", input_json(op.single_feedback));
  cJSON_AddItemToObject(feedback, "opened", input_json(op.opened_feedback));
  cJSON_AddItemToObject(feedback, "closed", input_json(op.closed_feedback));
  cJSON_AddItemToObject(feedback, "decoder", decoder_json(op.decoder));
  cJSON* timing = cJSON_AddObjectToObject(root, "timing");
  cJSON_AddNumberToObject(timing, "openingMs", config.timing.opening_ms);
  cJSON_AddNumberToObject(timing, "closingMs", config.timing.closing_ms);
  cJSON_AddNumberToObject(timing, "sensorReleaseTimeoutMs",
                          config.timing.sensor_release_timeout_ms);
  return root;
}

bool object(const cJSON* parent, const char* name, const cJSON** value,
            std::string* error) {
  *value = cJSON_GetObjectItemCaseSensitive(parent, name);
  if (*value != nullptr && cJSON_IsObject(*value)) return true;
  *error = std::string(name) + " must be an object.";
  return false;
}

bool array(const cJSON* parent, const char* name, const cJSON** value,
           std::string* error) {
  *value = cJSON_GetObjectItemCaseSensitive(parent, name);
  if (*value != nullptr && cJSON_IsArray(*value)) return true;
  *error = std::string(name) + " must be an array.";
  return false;
}

bool text(const cJSON* parent, const char* name, std::size_t maximum,
          std::string* value, std::string* error) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(parent, name);
  if (item == nullptr || !cJSON_IsString(item) || item->valuestring == nullptr ||
      std::char_traits<char>::length(item->valuestring) > maximum) {
    *error = std::string(name) + " must be a string of at most " +
             std::to_string(maximum) + " bytes.";
    return false;
  }
  *value = item->valuestring;
  return true;
}

bool integer(const cJSON* parent, const char* name, std::int64_t minimum,
             std::int64_t maximum, std::int64_t* value, std::string* error) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(parent, name);
  if (item == nullptr || !cJSON_IsNumber(item) ||
      !std::isfinite(item->valuedouble) ||
      std::floor(item->valuedouble) != item->valuedouble ||
      item->valuedouble < static_cast<double>(minimum) ||
      item->valuedouble > static_cast<double>(maximum)) {
    *error = std::string(name) + " must be an integer in the supported range.";
    return false;
  }
  *value = static_cast<std::int64_t>(item->valuedouble);
  return true;
}

bool boolean(const cJSON* parent, const char* name, bool* value,
             std::string* error) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(parent, name);
  if (item == nullptr || !cJSON_IsBool(item)) {
    *error = std::string(name) + " must be boolean.";
    return false;
  }
  *value = cJSON_IsTrue(item);
  return true;
}

bool parse_level_value(const std::string& value, ActiveLevel* level) {
  if (value == "low") *level = ActiveLevel::kLow;
  else if (value == "high") *level = ActiveLevel::kHigh;
  else return false;
  return true;
}

bool parse_pull_value(const std::string& value, SensorPull* pull) {
  if (value == "none") *pull = SensorPull::kNone;
  else if (value == "up") *pull = SensorPull::kUp;
  else if (value == "down") *pull = SensorPull::kDown;
  else return false;
  return true;
}

bool parse_output(const cJSON* value, PulseOutputConfig* output,
                  std::string* error) {
  std::int64_t gpio = 0, pulse = 0;
  std::string level;
  if (!integer(value, "gpio", -1, 39, &gpio, error) ||
      !text(value, "activeLevel", 4, &level, error) ||
      !integer(value, "pulseMs", 0, 10000, &pulse, error) ||
      !parse_level_value(level, &output->active_level)) {
    if (error->empty()) *error = "activeLevel must be low or high.";
    return false;
  }
  output->gpio = static_cast<int>(gpio);
  output->pulse_ms = static_cast<std::uint32_t>(pulse);
  return true;
}

bool parse_input(const cJSON* value, FeedbackInputConfig* input,
                 std::string* error) {
  std::int64_t gpio = 0, debounce = 0;
  std::string level, pull;
  if (!integer(value, "gpio", -1, 39, &gpio, error) ||
      !text(value, "activeLevel", 4, &level, error) ||
      !text(value, "pull", 4, &pull, error) ||
      !integer(value, "debounceMs", 0, 10000, &debounce, error) ||
      !parse_level_value(level, &input->active_level) ||
      !parse_pull_value(pull, &input->pull)) {
    if (error->empty()) *error = "Input level or pull is unsupported.";
    return false;
  }
  input->gpio = static_cast<int>(gpio);
  input->debounce_ms = static_cast<std::uint32_t>(debounce);
  return true;
}

bool parse_outcome(const std::string& value, RuleOutputConfig* output) {
  if (value == "opened" || value == "closed") {
    output->kind = RuleOutputKind::kPosition;
    output->position = value == "opened" ? PositionValue::kOpened
                                         : PositionValue::kClosed;
  } else if (value == "opening" || value == "closing" || value == "stopped") {
    output->kind = RuleOutputKind::kMovement;
    output->movement = value == "opening" ? MovementValue::kOpening
        : value == "closing" ? MovementValue::kClosing
                              : MovementValue::kStopped;
  } else if (value == "obstructed") {
    output->kind = RuleOutputKind::kFault;
    output->fault = FaultValue::kObstructed;
  } else return false;
  return true;
}

bool parse_expiry(const std::string& value, MatchAgeExpiry* expiry) {
  if (value == "none") *expiry = MatchAgeExpiry::kNone;
  else if (value == "unknown") *expiry = MatchAgeExpiry::kUnknown;
  else if (value == "obstructed") *expiry = MatchAgeExpiry::kObstructed;
  else if (value == "unknownAndObstructed") {
    *expiry = MatchAgeExpiry::kUnknownAndObstructed;
  } else return false;
  return true;
}

bool parse_decoder(const cJSON* value,
                   gate::config::FeedbackDecoderConfig* decoder,
                   std::string* error) {
  std::string profile;
  const cJSON* inputs = nullptr;
  const cJSON* rules = nullptr;
  if (!text(value, "profile", 32, &profile, error) ||
      !array(value, "inputs", &inputs, error) ||
      !array(value, "rules", &rules, error)) return false;
  if (profile == "endpointPreset") {
    decoder->profile = FeedbackDecoderProfile::kEndpointPreset;
  } else if (profile == "customRules") {
    decoder->profile = FeedbackDecoderProfile::kCustomRules;
  } else {
    *error = "decoder.profile is unsupported.";
    return false;
  }
  const int input_count = cJSON_GetArraySize(inputs);
  const int rule_count = cJSON_GetArraySize(rules);
  if (input_count < 0 || input_count > static_cast<int>(DecoderLimits::kMaxInputs) ||
      rule_count < 0 || rule_count > static_cast<int>(DecoderLimits::kMaxRules)) {
    *error = "Decoder input or rule count exceeds firmware limits.";
    return false;
  }
  *decoder = {};
  decoder->profile = profile == "customRules"
                         ? FeedbackDecoderProfile::kCustomRules
                         : FeedbackDecoderProfile::kEndpointPreset;
  decoder->input_count = static_cast<std::uint8_t>(input_count);
  decoder->rules.input_count = decoder->input_count;
  for (int i = 0; i < input_count; ++i) {
    const cJSON* item = cJSON_GetArrayItem(inputs, i);
    const cJSON* electrical = nullptr;
    std::int64_t id = 0;
    std::string label;
    if (!cJSON_IsObject(item) || !integer(item, "id", 0, 255, &id, error) ||
        !text(item, "label", 32, &label, error) || label.empty() ||
        !object(item, "electrical", &electrical, error) ||
        !parse_input(electrical, &decoder->inputs[i].electrical, error)) {
      if (error->empty()) *error = "Invalid decoder input.";
      return false;
    }
    decoder->inputs[i].id = static_cast<InputId>(id);
    decoder->inputs[i].label = std::move(label);
    decoder->rules.input_ids[i] = static_cast<InputId>(id);
  }
  decoder->rules.rule_count = static_cast<std::uint8_t>(rule_count);
  for (int r = 0; r < rule_count; ++r) {
    const cJSON* item = cJSON_GetArrayItem(rules, r);
    const cJSON* lifecycle = nullptr;
    const cJSON* groups = nullptr;
    std::int64_t id = 0, entry = 0, loss = 0, age = 0;
    std::string label, outcome, expiry;
    bool enabled = false;
    if (!cJSON_IsObject(item) || !integer(item, "id", 0, 255, &id, error) ||
        !text(item, "label", 32, &label, error) || label.empty() ||
        !boolean(item, "enabled", &enabled, error) ||
        !text(item, "outcome", 32, &outcome, error) ||
        !object(item, "lifecycle", &lifecycle, error) ||
        !integer(lifecycle, "entryConfirmationMs", 0,
                 DecoderLimits::kMaxDurationMs, &entry, error) ||
        !integer(lifecycle, "lossConfirmationMs", 0,
                 DecoderLimits::kMaxDurationMs, &loss, error) ||
        !integer(lifecycle, "matchAgeLimitMs", 0,
                 DecoderLimits::kMaxDurationMs, &age, error) ||
        !text(lifecycle, "matchAgeExpiry", 32, &expiry, error) ||
        !array(item, "groups", &groups, error)) return false;
    auto& rule = decoder->rules.rules[r];
    if (!parse_outcome(outcome, &rule.output) ||
        !parse_expiry(expiry, &rule.movement.expiry)) {
      *error = "Rule outcome or match-age expiry is unsupported.";
      return false;
    }
    rule.id = static_cast<RuleId>(id);
    rule.enabled = enabled;
    decoder->rule_labels[r] = std::move(label);
    rule.movement.entry_confirmation_ms = static_cast<Tick>(entry);
    rule.movement.loss_confirmation_ms = static_cast<Tick>(loss);
    rule.movement.match_age_limit_ms = static_cast<Tick>(age);
    const int group_count = cJSON_GetArraySize(groups);
    if (group_count < 1 ||
        group_count > static_cast<int>(DecoderLimits::kMaxGroupsPerRule)) {
      *error = "Each decoder rule requires a supported number of groups.";
      return false;
    }
    rule.group_count = static_cast<std::uint8_t>(group_count);
    for (int g = 0; g < group_count; ++g) {
      const cJSON* group = cJSON_GetArrayItem(groups, g);
      if (!cJSON_IsArray(group)) {
        *error = "Decoder groups must be arrays.";
        return false;
      }
      const int predicate_count = cJSON_GetArraySize(group);
      if (predicate_count < 1 || predicate_count >
              static_cast<int>(DecoderLimits::kMaxPredicatesPerGroup)) {
        *error = "Each decoder group requires a supported predicate count.";
        return false;
      }
      rule.groups[g].predicate_count = static_cast<std::uint8_t>(predicate_count);
      for (int p = 0; p < predicate_count; ++p) {
        const cJSON* item = cJSON_GetArrayItem(group, p);
        std::string kind;
        std::int64_t input_id = 0;
        if (!cJSON_IsObject(item) || !text(item, "kind", 32, &kind, error) ||
            !integer(item, "inputId", 0, 255, &input_id, error)) return false;
        auto& predicate = rule.groups[g].predicates[p];
        predicate.input_id = static_cast<InputId>(input_id);
        if (kind == "stableLevel") {
          std::int64_t level = 0, hold = 0;
          if (!integer(item, "level", 0, 1, &level, error) ||
              !integer(item, "holdMs", 1, DecoderLimits::kMaxDurationMs,
                       &hold, error)) return false;
          predicate.kind = PredicateKind::kStableLevel;
          predicate.stable = {level != 0, static_cast<Tick>(hold)};
        } else if (kind == "periodicEdges") {
          std::int64_t minimum = 0, maximum = 0, edges = 0, window = 0, gap = 0;
          if (!integer(item, "minimumIntervalMs", 1,
                       DecoderLimits::kMaxDurationMs, &minimum, error) ||
              !integer(item, "maximumIntervalMs", 1,
                       DecoderLimits::kMaxDurationMs, &maximum, error) ||
              !integer(item, "minimumEdges", 2,
                       DecoderLimits::kMaxEdgesPerInput, &edges, error) ||
              !integer(item, "observationWindowMs", 1,
                       DecoderLimits::kMaxDurationMs, &window, error) ||
              !integer(item, "maximumGapMs", 1,
                       DecoderLimits::kMaxDurationMs, &gap, error)) return false;
          predicate.kind = PredicateKind::kPeriodicEdges;
          predicate.periodic = {static_cast<Tick>(minimum),
                                static_cast<Tick>(maximum),
                                static_cast<std::uint8_t>(edges),
                                static_cast<Tick>(window),
                                static_cast<Tick>(gap)};
        } else {
          *error = "Unknown decoder predicate kind.";
          return false;
        }
      }
    }
  }
  return true;
}

}  // namespace

std::string serialize(const gate::config::AppConfig& config,
                      const Metadata& metadata) {
  Json root{build_json(config, metadata)};
  if (root.value == nullptr) return {};
  char* printed = cJSON_PrintUnformatted(root.value);
  if (printed == nullptr) return {};
  std::string result(printed);
  cJSON_free(printed);
  return result;
}

std::string sha256_hex(const std::string& value) {
  std::array<unsigned char, 32> digest{};
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.data()),
                     value.size(), digest.data(), 0) != 0) return {};
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(64, '0');
  for (std::size_t i = 0; i < digest.size(); ++i) {
    result[i * 2] = kHex[digest[i] >> 4];
    result[i * 2 + 1] = kHex[digest[i] & 0x0f];
  }
  return result;
}

esp_err_t parse(const std::string& json,
                const gate::config::AppConfig& current,
                Candidate* candidate,
                std::string* error) {
  if (candidate == nullptr || error == nullptr || json.empty() ||
      json.size() > kMaximumJsonSize) return ESP_ERR_INVALID_ARG;
  error->clear();
  const char* parse_end = nullptr;
  Json root{cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1,
                                     &parse_end, true)};
  if (root.value == nullptr || !cJSON_IsObject(root.value)) {
    *error = "Profile is not valid JSON.";
    return ESP_ERR_INVALID_ARG;
  }
  std::string format, project;
  std::int64_t version = 0, schema = 0;
  const cJSON* target = nullptr;
  const cJSON* compatibility = nullptr;
  const cJSON* op = nullptr;
  const cJSON* timing = nullptr;
  if (!text(root.value, "format", 64, &format, error) ||
      !integer(root.value, "version", 1, kVersion, &version, error) ||
      !text(root.value, "project", 64, &project, error) ||
      !object(root.value, "target", &target, error) ||
      !object(root.value, "compatibility", &compatibility, error) ||
      !integer(compatibility, "appSchemaVersion", 1,
               gate::config::kSchemaVersion, &schema, error) ||
      !object(root.value, "operator", &op, error) ||
      !object(root.value, "timing", &timing, error)) return ESP_ERR_INVALID_ARG;
  if (format != kFormat || project != kProject || version != kVersion ||
      schema != gate::config::kSchemaVersion) {
    *error = "Profile format, project, version, or schema is incompatible.";
    return ESP_ERR_NOT_SUPPORTED;
  }
  Candidate parsed;
  parsed.config = current;
  if (!text(target, "vendor", 64, &parsed.metadata.vendor, error) ||
      !text(target, "model", 64, &parsed.metadata.model, error) ||
      !text(target, "name", 64, &parsed.metadata.name, error) ||
      !text(target, "notes", 512, &parsed.metadata.notes, error)) {
    return ESP_ERR_INVALID_ARG;
  }
  std::string profile;
  std::int64_t minimum_interval = 0;
  const cJSON* outputs = nullptr;
  const cJSON* feedback = nullptr;
  if (!text(op, "profile", 32, &profile, error) ||
      !integer(op, "minimumIntervalMs", 0, 60000, &minimum_interval, error) ||
      !object(op, "outputs", &outputs, error) ||
      !object(op, "feedback", &feedback, error)) return ESP_ERR_INVALID_ARG;
  if (profile == "sequential") {
    parsed.config.gate_operator.profile = OperatorProfile::kSequential;
  } else if (profile == "directional") {
    parsed.config.gate_operator.profile = OperatorProfile::kDirectional;
  } else {
    *error = "operator.profile is unsupported.";
    return ESP_ERR_INVALID_ARG;
  }
  parsed.config.gate_operator.minimum_interval_ms =
      static_cast<std::uint32_t>(minimum_interval);
  const cJSON *step = nullptr, *open = nullptr, *close = nullptr;
  if (!object(outputs, "step", &step, error) ||
      !object(outputs, "open", &open, error) ||
      !object(outputs, "close", &close, error) ||
      !parse_output(step, &parsed.config.gate_operator.step, error) ||
      !parse_output(open, &parsed.config.gate_operator.open, error) ||
      !parse_output(close, &parsed.config.gate_operator.close, error)) {
    return ESP_ERR_INVALID_ARG;
  }
  std::string topology, endpoint;
  std::int64_t stability = 0;
  const cJSON *single = nullptr, *opened = nullptr, *closed = nullptr,
              *decoder = nullptr;
  if (!text(feedback, "topology", 16, &topology, error) ||
      !text(feedback, "singleActiveEndpoint", 16, &endpoint, error) ||
      !integer(feedback, "endpointStabilityMs", 0, 60000, &stability, error) ||
      !object(feedback, "single", &single, error) ||
      !object(feedback, "opened", &opened, error) ||
      !object(feedback, "closed", &closed, error) ||
      !object(feedback, "decoder", &decoder, error)) return ESP_ERR_INVALID_ARG;
  if (topology == "single") {
    parsed.config.gate_operator.feedback_topology = FeedbackTopology::kSingle;
  } else if (topology == "dual") {
    parsed.config.gate_operator.feedback_topology = FeedbackTopology::kDual;
  } else {
    *error = "feedback.topology is unsupported.";
    return ESP_ERR_INVALID_ARG;
  }
  if (endpoint == "open") {
    parsed.config.gate_operator.single_active_endpoint = FeedbackEndpoint::kOpen;
  } else if (endpoint == "closed") {
    parsed.config.gate_operator.single_active_endpoint = FeedbackEndpoint::kClosed;
  } else {
    *error = "feedback.singleActiveEndpoint is unsupported.";
    return ESP_ERR_INVALID_ARG;
  }
  parsed.config.gate_operator.endpoint_stability_ms =
      static_cast<std::uint32_t>(stability);
  if (!parse_input(single, &parsed.config.gate_operator.single_feedback, error) ||
      !parse_input(opened, &parsed.config.gate_operator.opened_feedback, error) ||
      !parse_input(closed, &parsed.config.gate_operator.closed_feedback, error) ||
      !parse_decoder(decoder, &parsed.config.gate_operator.decoder, error)) {
    return ESP_ERR_INVALID_ARG;
  }
  std::int64_t opening = 0, closing = 0, release = 0;
  if (!integer(timing, "openingMs", 0, 300000, &opening, error) ||
      !integer(timing, "closingMs", 0, 300000, &closing, error) ||
      !integer(timing, "sensorReleaseTimeoutMs", 0, 60000, &release, error)) {
    return ESP_ERR_INVALID_ARG;
  }
  parsed.config.timing.opening_ms = static_cast<std::uint32_t>(opening);
  parsed.config.timing.closing_ms = static_cast<std::uint32_t>(closing);
  parsed.config.timing.sensor_release_timeout_ms =
      static_cast<std::uint32_t>(release);
  const auto errors = gate::config::validate(parsed.config);
  if (!errors.empty()) {
    *error = errors.front().field + ": " + errors.front().message;
    return ESP_ERR_INVALID_ARG;
  }
  parsed.normalized_json = serialize(parsed.config, parsed.metadata);
  parsed.digest = sha256_hex(parsed.normalized_json);
  if (parsed.normalized_json.empty() || parsed.digest.empty()) {
    *error = "Could not normalize the profile.";
    return ESP_FAIL;
  }
  *candidate = std::move(parsed);
  return ESP_OK;
}

}  // namespace gate::profile
