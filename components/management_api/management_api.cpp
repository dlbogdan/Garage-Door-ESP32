#include "management_api.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "config_repository.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gate_hardware.hpp"
#include "gate_runtime.hpp"
#include "homespan_compatibility.hpp"
#include "password.hpp"
#include "web_auth.hpp"

namespace gate::management_api {
namespace {

constexpr char kTag[] = "management_api";
Context api_context{};

gate::config::AppConfig& config() { return *api_context.active_config; }
gate::bootstrap::Credentials& credentials() { return *api_context.credentials; }
bool provisioned() { return *api_context.application_provisioned; }

gate::config::ActiveLevel parse_level(const std::string& value) {
  return value == "high" ? gate::config::ActiveLevel::kHigh
                         : gate::config::ActiveLevel::kLow;
}

gate::config::SensorPull parse_pull(const std::string& value) {
  if (value == "none") return gate::config::SensorPull::kNone;
  if (value == "down") return gate::config::SensorPull::kDown;
  return gate::config::SensorPull::kUp;
}

bool form_value(const std::string& body, const char* key, std::string* value) {
  return api_context.form_value(body, key, value);
}
bool parse_integer(const std::string& body, const char* key, int minimum,
                   int maximum, int* value) {
  return api_context.parse_integer(body, key, minimum, maximum, value);
}
std::string json_escape(const char* input) { return api_context.json_escape(input); }
esp_err_t send_error(httpd_req_t* request, const char* status,
                     const std::string& message) {
  return api_context.send_error(request, status, message);
}

const char* decoder_health_name(gate::signal_decoder::DecoderHealth value) {
  using H = gate::signal_decoder::DecoderHealth;
  switch (value) {
    case H::kGatheringEvidence: return "gatheringEvidence";
    case H::kHealthy: return "healthy";
    case H::kAmbiguousPosition: return "ambiguousPosition";
    case H::kAmbiguousMovement: return "ambiguousMovement";
    case H::kMonitoringFailed: return "monitoringFailed";
  }
  return "monitoringFailed";
}

const char* movement_phase_name(gate::signal_decoder::MovementRulePhase value) {
  using P = gate::signal_decoder::MovementRulePhase;
  switch (value) {
    case P::kUnmatched: return "unmatched";
    case P::kEntryPending: return "entryPending";
    case P::kMatched: return "matched";
    case P::kLossPending: return "lossPending";
    case P::kExpired: return "expired";
  }
  return "unmatched";
}

std::string decoder_config_json(const gate::config::FeedbackDecoderConfig& decoder) {
  using namespace gate::signal_decoder;
  std::string json = "{\"version\":4,\"profile\":\"";
  json += decoder.profile == gate::config::FeedbackDecoderProfile::kCustomRules
              ? "customRules"
              : "endpointPreset";
  json += "\",\"limits\":{\"inputs\":4,\"rules\":8,\"groupsPerRule\":2,\"predicatesPerGroup\":3},\"inputs\":[";
  for (std::uint8_t i = 0; i < decoder.input_count; ++i) {
    if (i) json += ',';
    const auto& input = decoder.inputs[i];
    json += "{\"id\":" + std::to_string(input.id) + ",\"label\":\"" +
            json_escape(input.label.c_str()) + "\",\"gpio\":" +
            std::to_string(input.electrical.gpio) + ",\"activeLevel\":\"" +
            (input.electrical.active_level == gate::config::ActiveLevel::kHigh ? "high" : "low") +
            "\",\"pull\":\"" +
            (input.electrical.pull == gate::config::SensorPull::kNone ? "none" :
             input.electrical.pull == gate::config::SensorPull::kDown ? "down" : "up") +
            "\",\"debounceMs\":" + std::to_string(input.electrical.debounce_ms) + "}";
  }
  json += "],\"rules\":[";
  for (std::uint8_t r = 0; r < decoder.rules.rule_count; ++r) {
    if (r) json += ',';
    const auto& rule = decoder.rules.rules[r];
    std::string outcome;
    if (rule.output.kind == RuleOutputKind::kPosition) {
      outcome = rule.output.position == PositionValue::kOpened ? "opened" : "closed";
    } else if (rule.output.kind == RuleOutputKind::kMovement) {
      outcome = rule.output.movement == MovementValue::kOpening ? "opening" :
                rule.output.movement == MovementValue::kClosing ? "closing" : "stopped";
    } else outcome = "obstructed";
    const char* expiry = rule.movement.expiry == MatchAgeExpiry::kUnknown ? "unknown" :
                         rule.movement.expiry == MatchAgeExpiry::kObstructed ? "obstructed" :
                         rule.movement.expiry == MatchAgeExpiry::kUnknownAndObstructed ? "unknownAndObstructed" : "none";
    json += "{\"id\":" + std::to_string(rule.id) + ",\"label\":\"" +
            json_escape(decoder.rule_labels[r].c_str()) + "\",\"enabled\":" +
            (rule.enabled ? "true" : "false") + ",\"outcome\":\"" + outcome +
            "\",\"entryConfirmationMs\":" + std::to_string(rule.movement.entry_confirmation_ms) +
            ",\"lossConfirmationMs\":" + std::to_string(rule.movement.loss_confirmation_ms) +
            ",\"matchAgeLimitMs\":" + std::to_string(rule.movement.match_age_limit_ms) +
            ",\"matchAgeExpiry\":\"" + expiry + "\",\"groups\":[";
    for (std::uint8_t g = 0; g < rule.group_count; ++g) {
      if (g) json += ',';
      json += "[";
      for (std::uint8_t p = 0; p < rule.groups[g].predicate_count; ++p) {
        if (p) json += ',';
        const auto& predicate = rule.groups[g].predicates[p];
        json += "{\"kind\":\"";
        if (predicate.kind == PredicateKind::kStableLevel) {
          json += "stableLevel\",\"inputId\":" + std::to_string(predicate.input_id) +
                  ",\"level\":" + (predicate.stable.level ? "1" : "0") +
                  ",\"holdMs\":" + std::to_string(predicate.stable.hold_ms);
        } else {
          json += "periodicEdges\",\"inputId\":" + std::to_string(predicate.input_id) +
                  ",\"minimumIntervalMs\":" + std::to_string(predicate.periodic.minimum_interval_ms) +
                  ",\"maximumIntervalMs\":" + std::to_string(predicate.periodic.maximum_interval_ms) +
                  ",\"minimumEdges\":" + std::to_string(predicate.periodic.minimum_edges) +
                  ",\"observationWindowMs\":" + std::to_string(predicate.periodic.observation_window_ms) +
                  ",\"maximumGapMs\":" + std::to_string(predicate.periodic.maximum_gap_ms);
        }
        json += "}";
      }
      json += "]";
    }
    json += "]}";
  }
  return json + "]}";
}

esp_err_t login_handler(httpd_req_t* request) {
  if (!provisioned()) {
    return send_error(request, "409 Conflict", "Device is not provisioned.");
  }
  if (request->content_len <= 0 || request->content_len > 256) {
    return send_error(request, "400 Bad Request", "Invalid login request.");
  }
  std::string body(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (count <= 0) return ESP_FAIL;
    received += static_cast<std::size_t>(count);
  }
  std::string password;
  if (!form_value(body, "password", &password) ||
      !gate::config::verify_admin_password(password, config().admin)) {
    std::fill(password.begin(), password.end(), '\0');
    vTaskDelay(pdMS_TO_TICKS(500));
    return send_error(request, "401 Unauthorized", "Incorrect password.");
  }
  std::fill(password.begin(), password.end(), '\0');
  gate::web_auth::create_session();
  const std::string cookie = "gate_session=" + gate::web_auth::token() +
      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800";
  httpd_resp_set_hdr(request, "Set-Cookie", cookie.c_str());
  httpd_resp_set_type(request, "application/json");
  const std::string response = "{\"csrf\":\"" + gate::web_auth::csrf() + "\"}";
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t session_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request)) {
    return send_error(request, "401 Unauthorized", "Authentication required.");
  }
  const std::string response = "{\"authenticated\":true,\"csrf\":\"" +
                               gate::web_auth::csrf() + "\"}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t config_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request)) {
    return send_error(request, "401 Unauthorized", "Authentication required.");
  }
  const auto& op = config().gate_operator;
  const bool directional = op.profile == gate::config::OperatorProfile::kDirectional;
  const bool dual = op.feedback_topology == gate::config::FeedbackTopology::kDual;
  const bool custom_decoder =
      op.decoder.profile == gate::config::FeedbackDecoderProfile::kCustomRules;
  const auto& relay = directional ? op.open : op.step;
  const auto& sensor = dual ? op.opened_feedback : op.single_feedback;
  const std::string response =
      "{\"displayName\":\"" + json_escape(config().homekit.display_name.c_str()) +
      "\",\"ssid\":\"" + json_escape(config().wifi.ssid.c_str()) +
      "\",\"schemaVersion\":" +
      std::to_string(gate::config::kSchemaVersion) +
      ",\"operatorProfile\":\"" + std::string(directional ? "directional" : "sequential") + "\"" +
      ",\"feedbackMode\":\"" + std::string(dual ? "dual" : "single") + "\"" +
      ",\"feedbackDecoder\":\"" +
      std::string(custom_decoder ? "customRules" : "endpointPreset") + "\"" +
      ",\"decoderInputCount\":" + std::to_string(op.decoder.input_count) +
      ",\"decoderRuleCount\":" + std::to_string(op.decoder.rules.rule_count) +
      ",\"decoder\":" + decoder_config_json(op.decoder) +
      ",\"legacyFlatConfigLossy\":" +
      std::string(directional || dual || custom_decoder ? "true" : "false") +
      ",\"relayGpio\":" + std::to_string(relay.gpio) +
      ",\"relayActiveHigh\":" +
      std::string(relay.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"pulseMs\":" + std::to_string(relay.pulse_ms) +
      ",\"openGpio\":" + std::to_string(op.open.gpio) +
      ",\"openActiveHigh\":" + std::string(op.open.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"openPulseMs\":" + std::to_string(op.open.pulse_ms) +
      ",\"closeGpio\":" + std::to_string(op.close.gpio) +
      ",\"closeActiveHigh\":" + std::string(op.close.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"closePulseMs\":" + std::to_string(op.close.pulse_ms) +
      ",\"sensorGpio\":" + std::to_string(sensor.gpio) +
      ",\"sensorActiveHigh\":" +
      std::string(sensor.active_level == gate::config::ActiveLevel::kHigh ? "true" : "false") +
      ",\"sensorPull\":\"" +
      std::string(sensor.pull == gate::config::SensorPull::kNone ? "none" :
                   sensor.pull == gate::config::SensorPull::kDown ? "down" : "up") + "\"" +
      ",\"feedbackActiveEndpoint\":\"" +
      std::string(op.single_active_endpoint ==
                          gate::config::FeedbackEndpoint::kOpen
                      ? "open" : "closed") + "\"" +
      ",\"feedbackStabilityMs\":" +
      std::to_string(op.endpoint_stability_ms) +
      ",\"openingSeconds\":" + std::to_string(config().timing.opening_ms / 1000) +
      ",\"closingSeconds\":" + std::to_string(config().timing.closing_ms / 1000) +
      ",\"hardwareMonitoring\":" +
      std::string(gate::hardware::monitoring_active() ? "true" : "false") +
      ",\"feedbackActive\":" +
      std::string((gate::hardware::feedback_assertions().opened || gate::hardware::feedback_assertions().closed) ? "true" : "false") +
      ",\"relayControlEnabled\":" +
      std::string(gate::runtime::active() ? "true" : "false") + "}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t runtime_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, false, false)) {
    return send_error(request, "401 Unauthorized", "Authentication required.");
  }
  const auto state = gate::runtime::snapshot();
  const auto assertions = gate::hardware::feedback_assertions();
  const auto custom_sample = gate::hardware::feedback_sample();
  const auto decoder = gate::runtime::decoder_snapshot();
  std::string decoder_inputs = "[";
  for (std::uint8_t index = 0; index < custom_sample.count; ++index) {
    if (index != 0) decoder_inputs += ',';
    decoder_inputs += "{\"id\":" +
                      std::to_string(custom_sample.levels[index].id) +
                      ",\"level\":" +
                      (custom_sample.levels[index].level ? "true" : "false") +
                      "}";
  }
  decoder_inputs += ']';
  std::string decoder_rules = "[";
  for (std::uint8_t index = 0; index < decoder.diagnostics.rule_count; ++index) {
    if (index) decoder_rules += ',';
    const auto& rule = decoder.diagnostics.rules[index];
    decoder_rules += "{\"id\":" + std::to_string(rule.id) +
                     ",\"expressionValue\":" + (rule.expression_value ? "true" : "false") +
                     ",\"outputAsserted\":" + (rule.output_asserted ? "true" : "false") +
                     ",\"phase\":\"" + movement_phase_name(rule.movement_phase) +
                     "\",\"matchAgeMs\":" + std::to_string(rule.match_age_ms) + "}";
  }
  decoder_rules += ']';
  std::string decoder_predicates = "[";
  for (std::uint8_t index = 0; index < decoder.diagnostics.predicate_count;
       ++index) {
    if (index) decoder_predicates += ',';
    const auto& predicate = decoder.diagnostics.predicates[index];
    decoder_predicates +=
        "{\"index\":" + std::to_string(index) +
        ",\"value\":" + (predicate.value ? "true" : "false") +
        ",\"evidenceValid\":" +
        (predicate.evidence_valid ? "true" : "false") +
        ",\"evidenceAgeMs\":" +
        std::to_string(predicate.evidence_age_ms) +
        ",\"latestIntervalMs\":" +
        std::to_string(predicate.latest_interval_ms) +
        ",\"estimatedCycleMs\":" +
        std::to_string(predicate.latest_interval_ms * 2U) +
        ",\"qualifyingEdgeCount\":" +
        std::to_string(predicate.qualifying_edge_count) + "}";
  }
  decoder_predicates += ']';
  const std::string response =
      "{\"hardwareMonitoring\":" +
      std::string(gate::hardware::monitoring_active() ? "true" : "false") +
      ",\"feedbackActive\":" +
      std::string((assertions.opened || assertions.closed) ? "true" : "false") +
      ",\"openedAsserted\":" + std::string(assertions.opened ? "true" : "false") +
      ",\"closedAsserted\":" + std::string(assertions.closed ? "true" : "false") +
      ",\"observation\":\"" + std::string(state.observation_valid ? gate::controller::to_string(state.observation) : "UNKNOWN") + "\"" +
      ",\"state\":\"" + std::string(gate::controller::to_string(state.state)) + "\"" +
      ",\"target\":\"" + std::string(gate::controller::to_string(state.target)) + "\"" +
      ",\"activeCommand\":\"" + std::string(gate::controller::to_string(state.active_command)) + "\"" +
      ",\"pulseActive\":" + std::string(state.pulse_active ? "true" : "false") +
      ",\"obstruction\":" + std::string(state.obstruction ? "true" : "false") +
      ",\"faultReason\":\"" + std::string(gate::controller::to_string(state.fault)) + "\"" +
      ",\"decoderInputs\":" + decoder_inputs +
      ",\"decoderSampleTimestampMs\":" +
      std::to_string(custom_sample.timestamp_ms) +
      ",\"decoderActive\":" + (decoder.active ? "true" : "false") +
      ",\"decoderHealth\":\"" + decoder_health_name(decoder.result.health) + "\"" +
      ",\"decoderGeneration\":" + std::to_string(decoder.result.generation) +
      ",\"decoderPosition\":\"" +
      (decoder.result.position_valid
           ? (decoder.result.position == gate::signal_decoder::PositionValue::kOpened ? "opened" : "closed")
           : "unknown") + "\"" +
      ",\"decoderMovement\":\"" +
      (decoder.result.movement_valid
           ? (decoder.result.movement == gate::signal_decoder::MovementValue::kOpening ? "opening" :
              decoder.result.movement == gate::signal_decoder::MovementValue::kClosing ? "closing" : "stopped")
           : "unknown") + "\"" +
      ",\"decoderObstructed\":" + (decoder.result.obstructed ? "true" : "false") +
      ",\"decoderRules\":" + decoder_rules +
      ",\"decoderPredicates\":" + decoder_predicates +
      ",\"relayControlEnabled\":" +
      std::string(gate::runtime::active() ? "true" : "false") + "}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t relay_pulse_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  switch (gate::runtime::request_bench_pulse()) {
    case gate::runtime::RequestResult::kAccepted:
      httpd_resp_set_status(request, "202 Accepted");
      httpd_resp_set_type(request, "application/json");
      return httpd_resp_sendstr(request, "{\"accepted\":true}");
    case gate::runtime::RequestResult::kBusy:
      return send_error(request, "409 Conflict",
                        "Relay pulse is active or cooling down. Try again shortly.");
    case gate::runtime::RequestResult::kHardwareError:
      return send_error(request, "500 Internal Server Error",
                        "Relay GPIO activation failed.");
    case gate::runtime::RequestResult::kUnavailable:
      return send_error(request, "503 Service Unavailable",
                        "Gate runtime is not available.");
  }
  return ESP_FAIL;
}

esp_err_t gate_target_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  const bool opening = request->user_ctx == nullptr;
  const auto target = opening ? gate::controller::Target::kOpen
                              : gate::controller::Target::kClosed;
  switch (gate::runtime::request_target(target)) {
    case gate::runtime::RequestResult::kAccepted:
      httpd_resp_set_status(request, "202 Accepted");
      httpd_resp_set_type(request, "application/json");
      return httpd_resp_sendstr(request, opening
          ? "{\"accepted\":true,\"target\":\"OPEN\"}"
          : "{\"accepted\":true,\"target\":\"CLOSED\"}");
    case gate::runtime::RequestResult::kBusy:
      return send_error(request, "409 Conflict",
                        opening
                            ? "The open command is interlocked or the operator is busy."
                            : "The close command is interlocked or the operator is busy.");
    case gate::runtime::RequestResult::kHardwareError:
      return send_error(request, "500 Internal Server Error",
                        "The configured operator output could not be activated.");
    case gate::runtime::RequestResult::kUnavailable:
      return send_error(request, "503 Service Unavailable",
                        "Gate runtime is not available.");
  }
  return ESP_FAIL;
}

esp_err_t homekit_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request)) {
    return send_error(request, "401 Unauthorized", "Authentication required.");
  }
  const std::string& code = config().homekit.setup_code;
  const std::string formatted_code =
      code.size() == 8 ? code.substr(0, 3) + "-" + code.substr(3, 2) + "-" +
                             code.substr(5, 3)
                       : code;
  const std::string response =
      "{\"active\":" +
      std::string(gate::homekit::active() ? "true" : "false") +
      ",\"paired\":" +
      std::string(gate::homekit::paired() ? "true" : "false") +
      ",\"setupCode\":\"" + json_escape(formatted_code.c_str()) +
      "\",\"setupId\":\"" +
      json_escape(config().homekit.setup_id.c_str()) + "\"}";
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, response.c_str(), response.size());
}

esp_err_t update_config_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  if (request->content_len <= 0 || request->content_len > 16384) {
    return send_error(request, "400 Bad Request", "Invalid settings request.");
  }
  std::string body(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (count <= 0) return ESP_FAIL;
    received += static_cast<std::size_t>(count);
  }

  std::string display_name;
  std::string relay_level;
  std::string sensor_level;
  std::string sensor_pull;
  std::string feedback_endpoint;
  int relay_gpio = -1;
  int sensor_gpio = -1;
  int pulse_ms = 0;
  int opening_seconds = 0;
  int closing_seconds = 0;
  int feedback_stability_ms = 0;
  std::string operator_profile;
  std::string feedback_mode;
  std::string feedback_decoder;
  const bool has_operator_profile =
      form_value(body, "operatorProfile", &operator_profile);
  const bool has_feedback_mode = form_value(body, "feedbackMode", &feedback_mode);
  const bool has_feedback_decoder =
      form_value(body, "feedbackDecoder", &feedback_decoder);
  const bool canonical = has_operator_profile && has_feedback_mode;
  if (has_operator_profile != has_feedback_mode) {
    return send_error(request, "400 Bad Request",
                      "Operator profile and feedback topology must both be provided.");
  }
  if (!form_value(body, "displayName", &display_name) || display_name.empty() ||
      display_name.size() > 64 ||
      !parse_integer(body, "openingSeconds", 3, 180, &opening_seconds) ||
      !parse_integer(body, "closingSeconds", 3, 180, &closing_seconds) ||
      !parse_integer(body, "feedbackStabilityMs", 1000, 10000,
                     &feedback_stability_ms)) {
    return send_error(request, "400 Bad Request",
                      "One or more settings have an invalid format.");
  }

  gate::config::AppConfig updated = config();
  updated.homekit.display_name = display_name;
  if (!canonical) {
    if (!form_value(body, "relayLevel", &relay_level) ||
        !form_value(body, "sensorLevel", &sensor_level) ||
        !form_value(body, "sensorPull", &sensor_pull) ||
        !form_value(body, "feedbackEndpoint", &feedback_endpoint) ||
        !parse_integer(body, "relayGpio", 0, 39, &relay_gpio) ||
        !parse_integer(body, "sensorGpio", 0, 39, &sensor_gpio) ||
        !parse_integer(body, "pulseMs", 100, 2000, &pulse_ms) ||
        (relay_level != "low" && relay_level != "high") ||
        (sensor_level != "low" && sensor_level != "high") ||
        (sensor_pull != "none" && sensor_pull != "up" && sensor_pull != "down") ||
        (feedback_endpoint != "open" && feedback_endpoint != "closed")) {
      return send_error(request, "400 Bad Request", "Invalid legacy gate settings.");
    }
    updated.gate_operator.profile = gate::config::OperatorProfile::kSequential;
    updated.gate_operator.feedback_topology = gate::config::FeedbackTopology::kSingle;
    updated.gate_operator.step = {relay_gpio, parse_level(relay_level),
                                  static_cast<std::uint32_t>(pulse_ms)};
    updated.gate_operator.single_feedback = {
        sensor_gpio, parse_level(sensor_level), parse_pull(sensor_pull), 50};
    updated.gate_operator.single_active_endpoint =
        feedback_endpoint == "open" ? gate::config::FeedbackEndpoint::kOpen
                                    : gate::config::FeedbackEndpoint::kClosed;
  } else {
    if ((operator_profile != "sequential" && operator_profile != "directional") ||
        (feedback_mode != "single" && feedback_mode != "dual")) {
      return send_error(request, "400 Bad Request", "Invalid operator discriminators.");
    }
    updated.gate_operator.profile = operator_profile == "directional"
        ? gate::config::OperatorProfile::kDirectional
        : gate::config::OperatorProfile::kSequential;
    updated.gate_operator.feedback_topology = feedback_mode == "dual"
        ? gate::config::FeedbackTopology::kDual
        : gate::config::FeedbackTopology::kSingle;

    if (has_feedback_decoder && feedback_decoder != "endpointPreset" &&
        feedback_decoder != "customRules") {
      return send_error(request, "400 Bad Request", "Invalid feedback decoder profile.");
    }
    if (!has_feedback_decoder &&
        updated.gate_operator.decoder.profile ==
            gate::config::FeedbackDecoderProfile::kCustomRules) {
      return send_error(request, "409 Conflict",
                        "This configuration uses custom decoder rules; use the version 4 decoder form to edit it.");
    }
    const bool custom = has_feedback_decoder && feedback_decoder == "customRules";
    updated.gate_operator.decoder.profile =
        custom ? gate::config::FeedbackDecoderProfile::kCustomRules
               : gate::config::FeedbackDecoderProfile::kEndpointPreset;

    auto parse_output = [&](const char* gpio_name, const char* level_name,
                            const char* pulse_name,
                            gate::config::PulseOutputConfig* output) {
      std::string level;
      int gpio = -1;
      int pulse = 0;
      if (!parse_integer(body, gpio_name, 0, 39, &gpio) ||
          !form_value(body, level_name, &level) ||
          !parse_integer(body, pulse_name, 100, 2000, &pulse) ||
          (level != "low" && level != "high")) return false;
      *output = {gpio, parse_level(level), static_cast<std::uint32_t>(pulse)};
      return true;
    };
    if (updated.gate_operator.profile == gate::config::OperatorProfile::kSequential) {
      if (!parse_output("stepGpio", "stepLevel", "stepPulseMs",
                        &updated.gate_operator.step)) {
        return send_error(request, "400 Bad Request", "Invalid STEP output.");
      }
    } else if (!parse_output("openGpio", "openLevel", "openPulseMs",
                             &updated.gate_operator.open) ||
               !parse_output("closeGpio", "closeLevel", "closePulseMs",
                             &updated.gate_operator.close)) {
      return send_error(request, "400 Bad Request", "Invalid directional outputs.");
    }

    auto parse_input = [&](const char* gpio_name, const char* level_name,
                           const char* pull_name,
                           gate::config::FeedbackInputConfig* input) {
      std::string level;
      std::string pull;
      int gpio = -1;
      if (!parse_integer(body, gpio_name, 0, 39, &gpio) ||
          !form_value(body, level_name, &level) ||
          !form_value(body, pull_name, &pull) ||
          (level != "low" && level != "high") ||
          (pull != "none" && pull != "up" && pull != "down")) return false;
      *input = {gpio, parse_level(level), parse_pull(pull), 50};
      return true;
    };
    if (!custom && updated.gate_operator.feedback_topology == gate::config::FeedbackTopology::kSingle) {
      if (!form_value(body, "feedbackEndpoint", &feedback_endpoint) ||
          (feedback_endpoint != "open" && feedback_endpoint != "closed") ||
          !parse_input("sensorGpio", "sensorLevel", "sensorPull",
                       &updated.gate_operator.single_feedback)) {
        return send_error(request, "400 Bad Request", "Invalid single feedback input.");
      }
      updated.gate_operator.single_active_endpoint =
          feedback_endpoint == "open" ? gate::config::FeedbackEndpoint::kOpen
                                      : gate::config::FeedbackEndpoint::kClosed;
    } else if (!custom && (!parse_input("openedSensorGpio", "openedSensorLevel",
                            "openedSensorPull", &updated.gate_operator.opened_feedback) ||
               !parse_input("closedSensorGpio", "closedSensorLevel",
                             "closedSensorPull", &updated.gate_operator.closed_feedback))) {
      return send_error(request, "400 Bad Request", "Invalid dual feedback inputs.");
    }

    if (custom) {
      using namespace gate::signal_decoder;
      gate::config::FeedbackDecoderConfig parsed;
      parsed.profile = gate::config::FeedbackDecoderProfile::kCustomRules;
      int input_count = 0;
      int rule_count = 0;
      if (!parse_integer(body, "decoderInputCount", 1,
                         DecoderLimits::kMaxInputs, &input_count) ||
          !parse_integer(body, "decoderRuleCount", 1,
                         DecoderLimits::kMaxRules, &rule_count)) {
        return send_error(request, "400 Bad Request", "Invalid decoder counts.");
      }
      parsed.input_count = static_cast<std::uint8_t>(input_count);
      parsed.rules.input_count = parsed.input_count;
      for (int i = 0; i < input_count; ++i) {
        const std::string prefix = "decoderInput" + std::to_string(i);
        std::string label, level, pull;
        int id = 0, gpio = 0, debounce = 0;
        if (!parse_integer(body, (prefix + "Id").c_str(), 0, 255, &id) ||
            !form_value(body, (prefix + "Label").c_str(), &label) ||
            label.empty() || label.size() > 32 ||
            !parse_integer(body, (prefix + "Gpio").c_str(), 0, 39, &gpio) ||
            !form_value(body, (prefix + "Level").c_str(), &level) ||
            !form_value(body, (prefix + "Pull").c_str(), &pull) ||
            !parse_integer(body, (prefix + "DebounceMs").c_str(), 10, 500, &debounce) ||
            (level != "low" && level != "high") ||
            (pull != "none" && pull != "up" && pull != "down")) {
          return send_error(request, "400 Bad Request", "Invalid declared decoder input.");
        }
        parsed.inputs[i] = {static_cast<InputId>(id), label,
                            {gpio, parse_level(level), parse_pull(pull),
                             static_cast<std::uint32_t>(debounce)}};
        parsed.rules.input_ids[i] = static_cast<InputId>(id);
      }
      parsed.rules.rule_count = static_cast<std::uint8_t>(rule_count);
      for (int r = 0; r < rule_count; ++r) {
        const std::string prefix = "decoderRule" + std::to_string(r);
        auto& rule = parsed.rules.rules[r];
        std::string label, outcome, expiry;
        int id = 0, group_count = 0, entry = 0, loss = 0, age = 0;
        if (!parse_integer(body, (prefix + "Id").c_str(), 0, 255, &id) ||
            !form_value(body, (prefix + "Label").c_str(), &label) ||
            label.empty() || label.size() > 32 ||
            !form_value(body, (prefix + "Outcome").c_str(), &outcome) ||
            !parse_integer(body, (prefix + "GroupCount").c_str(), 1,
                           DecoderLimits::kMaxGroupsPerRule, &group_count) ||
            !parse_integer(body, (prefix + "EntryMs").c_str(), 0,
                           std::numeric_limits<int>::max(), &entry) ||
            !parse_integer(body, (prefix + "LossMs").c_str(), 0,
                           std::numeric_limits<int>::max(), &loss) ||
            !parse_integer(body, (prefix + "AgeMs").c_str(), 0,
                           std::numeric_limits<int>::max(), &age) ||
            !form_value(body, (prefix + "Expiry").c_str(), &expiry)) {
          return send_error(request, "400 Bad Request", "Invalid decoder rule.");
        }
        rule.id = static_cast<RuleId>(id);
        rule.enabled = true;
        parsed.rule_labels[r] = label;
        rule.group_count = static_cast<std::uint8_t>(group_count);
        if (outcome == "opened" || outcome == "closed") {
          rule.output.kind = RuleOutputKind::kPosition;
          rule.output.position = outcome == "opened" ? PositionValue::kOpened : PositionValue::kClosed;
        } else if (outcome == "opening" || outcome == "closing" || outcome == "stopped") {
          rule.output.kind = RuleOutputKind::kMovement;
          rule.output.movement = outcome == "opening" ? MovementValue::kOpening :
                                 outcome == "closing" ? MovementValue::kClosing : MovementValue::kStopped;
          rule.movement = {static_cast<Tick>(entry), static_cast<Tick>(loss),
                           static_cast<Tick>(age),
                           expiry == "unknown" ? MatchAgeExpiry::kUnknown :
                           expiry == "obstructed" ? MatchAgeExpiry::kObstructed :
                           expiry == "unknownAndObstructed" ? MatchAgeExpiry::kUnknownAndObstructed : MatchAgeExpiry::kNone};
        } else if (outcome == "obstructed") {
          rule.output.kind = RuleOutputKind::kFault;
        } else {
          return send_error(request, "400 Bad Request", "Invalid decoder outcome.");
        }
        for (int g = 0; g < group_count; ++g) {
          const std::string group_prefix = prefix + "Group" + std::to_string(g);
          int predicate_count = 0;
          if (!parse_integer(body, (group_prefix + "PredicateCount").c_str(), 1,
                             DecoderLimits::kMaxPredicatesPerGroup, &predicate_count)) {
            return send_error(request, "400 Bad Request", "Invalid decoder alternative group.");
          }
          rule.groups[g].predicate_count = static_cast<std::uint8_t>(predicate_count);
          for (int p = 0; p < predicate_count; ++p) {
            const std::string pp = group_prefix + "Predicate" + std::to_string(p);
            std::string kind;
            int input_id = 0;
            if (!form_value(body, (pp + "Kind").c_str(), &kind) ||
                !parse_integer(body, (pp + "InputId").c_str(), 0, 255, &input_id)) {
              return send_error(request, "400 Bad Request", "Invalid decoder predicate.");
            }
            auto& predicate = rule.groups[g].predicates[p];
            predicate.input_id = static_cast<InputId>(input_id);
            if (kind == "stableLevel") {
              int level_value = 0, hold = 0;
              if (!parse_integer(body, (pp + "Level").c_str(), 0, 1, &level_value) ||
                  !parse_integer(body, (pp + "HoldMs").c_str(), 1,
                                 std::numeric_limits<int>::max(), &hold)) {
                return send_error(request, "400 Bad Request", "Invalid stable-level predicate.");
              }
              predicate.kind = PredicateKind::kStableLevel;
              predicate.stable = {level_value != 0, static_cast<Tick>(hold)};
            } else if (kind == "periodicEdges") {
              int minimum = 0, maximum = 0, edges = 0, window = 0, gap = 0;
              if (!parse_integer(body, (pp + "MinimumIntervalMs").c_str(), 1, std::numeric_limits<int>::max(), &minimum) ||
                  !parse_integer(body, (pp + "MaximumIntervalMs").c_str(), 1, std::numeric_limits<int>::max(), &maximum) ||
                  !parse_integer(body, (pp + "MinimumEdges").c_str(), 2, DecoderLimits::kMaxEdgesPerInput, &edges) ||
                  !parse_integer(body, (pp + "ObservationWindowMs").c_str(), 1, std::numeric_limits<int>::max(), &window) ||
                  !parse_integer(body, (pp + "MaximumGapMs").c_str(), 1, std::numeric_limits<int>::max(), &gap)) {
                return send_error(request, "400 Bad Request", "Invalid periodic predicate.");
              }
              predicate.kind = PredicateKind::kPeriodicEdges;
              predicate.periodic = {static_cast<Tick>(minimum), static_cast<Tick>(maximum),
                                    static_cast<std::uint8_t>(edges), static_cast<Tick>(window),
                                    static_cast<Tick>(gap)};
            } else return send_error(request, "400 Bad Request", "Unknown decoder predicate kind.");
          }
        }
      }
      updated.gate_operator.decoder = std::move(parsed);
    }
  }
  updated.gate_operator.endpoint_stability_ms = feedback_stability_ms;
  updated.timing.opening_ms = opening_seconds * 1000;
  updated.timing.closing_ms = closing_seconds * 1000;
  const auto errors = gate::config::validate(updated);
  if (!errors.empty()) {
    return send_error(request, "400 Bad Request",
                      errors.front().field + ": " + errors.front().message);
  }
  const esp_err_t result = gate::config::ConfigRepository().save(updated);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Failed to save authenticated settings: %s",
             esp_err_to_name(result));
    return send_error(request, "500 Internal Server Error",
                      "Could not persist settings.");
  }
  config() = std::move(updated);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_sendstr(request, "{\"saved\":true,\"restarting\":true}");
  // Hardware GPIO ownership, compiled decoder tables, and runtime histories are
  // startup-owned. Applying only the in-memory AppConfig would leave the active
  // board on the old wiring/rules, so activate every gate configuration change
  // through one controlled restart.
  api_context.schedule_restart();
  return ESP_OK;
}

esp_err_t password_change_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  if (request->content_len <= 0 || request->content_len > 512) {
    return send_error(request, "400 Bad Request", "Invalid password request.");
  }
  std::string body(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (count <= 0) return ESP_FAIL;
    received += static_cast<std::size_t>(count);
  }

  std::string current_password;
  std::string new_password;
  std::string confirmation;
  const bool valid_format =
      form_value(body, "currentPassword", &current_password) &&
      form_value(body, "newPassword", &new_password) &&
      form_value(body, "confirmation", &confirmation) &&
      new_password.size() >= 10 && new_password.size() <= 128 &&
      new_password == confirmation;
  std::fill(confirmation.begin(), confirmation.end(), '\0');
  if (!valid_format) {
    std::fill(current_password.begin(), current_password.end(), '\0');
    std::fill(new_password.begin(), new_password.end(), '\0');
    return send_error(request, "400 Bad Request",
                      "New passwords must match and contain 10-128 characters.");
  }
  if (!gate::config::verify_admin_password(current_password,
                                            config().admin)) {
    std::fill(current_password.begin(), current_password.end(), '\0');
    std::fill(new_password.begin(), new_password.end(), '\0');
    vTaskDelay(pdMS_TO_TICKS(500));
    return send_error(request, "401 Unauthorized",
                      "Current administrator password is incorrect.");
  }
  std::fill(current_password.begin(), current_password.end(), '\0');

  gate::config::AppConfig updated = config();
  esp_err_t result = gate::config::derive_admin_password(new_password,
                                                          &updated.admin);
  std::fill(new_password.begin(), new_password.end(), '\0');
  if (result != ESP_OK) {
    return send_error(request, "500 Internal Server Error",
                      "Could not derive new administrator credentials.");
  }
  result = gate::config::ConfigRepository().save(updated);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Failed to save administrator password: %s",
             esp_err_to_name(result));
    return send_error(request, "500 Internal Server Error",
                      "Could not persist the new password.");
  }

  config() = std::move(updated);
  gate::web_auth::clear_session();
  httpd_resp_set_hdr(request, "Set-Cookie",
                     "gate_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"changed\":true}");
}

esp_err_t wifi_change_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  if (request->content_len <= 0 || request->content_len > 512) {
    return send_error(request, "400 Bad Request", "Invalid network request.");
  }
  std::string body(request->content_len, '\0');
  std::size_t received = 0;
  while (received < body.size()) {
    const int count = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (count <= 0) return ESP_FAIL;
    received += static_cast<std::size_t>(count);
  }

  std::string ssid;
  std::string wifi_password;
  std::string admin_password;
  const bool valid_format =
      form_value(body, "ssid", &ssid) &&
      form_value(body, "wifiPassword", &wifi_password) &&
      form_value(body, "adminPassword", &admin_password) &&
      !ssid.empty() && ssid.size() <= 32 && wifi_password.size() <= 63 &&
      (wifi_password.empty() || wifi_password.size() >= 8);
  if (!valid_format) {
    std::fill(wifi_password.begin(), wifi_password.end(), '\0');
    std::fill(admin_password.begin(), admin_password.end(), '\0');
    return send_error(request, "400 Bad Request",
                      "Enter an SSID and either no password or 8-63 characters.");
  }
  if (!gate::config::verify_admin_password(admin_password,
                                            config().admin)) {
    std::fill(wifi_password.begin(), wifi_password.end(), '\0');
    std::fill(admin_password.begin(), admin_password.end(), '\0');
    vTaskDelay(pdMS_TO_TICKS(500));
    return send_error(request, "401 Unauthorized",
                      "Administrator password is incorrect.");
  }
  std::fill(admin_password.begin(), admin_password.end(), '\0');

  gate::config::AppConfig previous = config();
  gate::config::AppConfig updated = config();
  updated.wifi.ssid = ssid;
  updated.wifi.password = wifi_password;
  const auto errors = gate::config::validate(updated);
  if (!errors.empty()) {
    std::fill(wifi_password.begin(), wifi_password.end(), '\0');
    return send_error(request, "400 Bad Request",
                      errors.front().field + ": " + errors.front().message);
  }

  esp_err_t result = gate::config::ConfigRepository().save(updated);
  if (result != ESP_OK) {
    std::fill(wifi_password.begin(), wifi_password.end(), '\0');
    return send_error(request, "500 Internal Server Error",
                      "Could not persist the network configuration.");
  }

  const gate::bootstrap::Credentials previous_credentials = credentials();
  gate::bootstrap::set_station(&credentials(), ssid, wifi_password);
  std::fill(wifi_password.begin(), wifi_password.end(), '\0');
  result = gate::bootstrap::save(credentials());
  if (result != ESP_OK) {
    credentials() = previous_credentials;
    const esp_err_t rollback = gate::config::ConfigRepository().save(previous);
    if (rollback != ESP_OK) {
      ESP_LOGE(kTag, "Wi-Fi update rollback failed: %s",
               esp_err_to_name(rollback));
    }
    return send_error(request, "500 Internal Server Error",
                      "Could not persist bootstrap network credentials.");
  }

  config() = std::move(updated);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_sendstr(request, "{\"saved\":true,\"restarting\":true}");
  api_context.schedule_restart();
  return ESP_OK;
}

esp_err_t logout_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  gate::web_auth::clear_session();
  httpd_resp_set_hdr(request, "Set-Cookie",
                     "gate_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  return httpd_resp_sendstr(request, "{}");
}

esp_err_t reboot_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  httpd_resp_set_type(request, "application/json");
  httpd_resp_sendstr(request, "{\"restarting\":true}");
  api_context.schedule_restart();
  return ESP_OK;
}

esp_err_t homekit_reset_handler(httpd_req_t* request) {
  if (!gate::web_auth::authorize(request, true)) {
    return send_error(request, "403 Forbidden", "Invalid session or CSRF token.");
  }
  const esp_err_t result = gate::homekit::reset_pairings();
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Could not reset Apple Home pairings: %s",
             esp_err_to_name(result));
    return send_error(request, "409 Conflict",
                      "Apple Home service is not active; restart and try again.");
  }
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request,
                            "{\"reset\":true,\"paired\":false}");
}

}  // namespace

esp_err_t register_routes(httpd_handle_t server, Context context) {
  if (server == nullptr || context.credentials == nullptr ||
      context.active_config == nullptr || context.application_provisioned == nullptr ||
      context.send_error == nullptr || context.form_value == nullptr ||
      context.parse_integer == nullptr || context.json_escape == nullptr ||
      context.schedule_restart == nullptr) return ESP_ERR_INVALID_ARG;
  api_context = context;
  const httpd_uri_t routes[] = {
      {.uri = "/api/v1/session/login", .method = HTTP_POST, .handler = login_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/session", .method = HTTP_GET, .handler = session_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/config", .method = HTTP_GET, .handler = config_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/runtime", .method = HTTP_GET, .handler = runtime_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/gate/test-pulse", .method = HTTP_POST, .handler = relay_pulse_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/gate/open", .method = HTTP_POST, .handler = gate_target_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/gate/close", .method = HTTP_POST, .handler = gate_target_handler, .user_ctx = reinterpret_cast<void*>(1)},
      {.uri = "/api/v1/homekit", .method = HTTP_GET, .handler = homekit_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/homekit/pairings", .method = HTTP_DELETE, .handler = homekit_reset_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/config", .method = HTTP_PUT, .handler = update_config_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/access/password", .method = HTTP_PUT, .handler = password_change_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/network/wifi", .method = HTTP_PUT, .handler = wifi_change_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/session/logout", .method = HTTP_POST, .handler = logout_handler, .user_ctx = nullptr},
      {.uri = "/api/v1/system/reboot", .method = HTTP_POST, .handler = reboot_handler, .user_ctx = nullptr},
  };
  for (const auto& route : routes) {
    const esp_err_t result = httpd_register_uri_handler(server, &route);
    if (result != ESP_OK) return result;
  }
  return ESP_OK;
}

}  // namespace gate::management_api
