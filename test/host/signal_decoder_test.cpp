#include "signal_decoder.hpp"

#include <cstdlib>
#include <iostream>

using namespace gate::signal_decoder;

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

PredicateConfig stable(InputId input, bool level, Tick hold) {
  PredicateConfig predicate;
  predicate.kind = PredicateKind::kStableLevel;
  predicate.input_id = input;
  predicate.stable = {level, hold};
  return predicate;
}

PredicateConfig periodic(InputId input, Tick minimum, Tick maximum,
                         std::uint8_t edges, Tick window, Tick gap) {
  PredicateConfig predicate;
  predicate.kind = PredicateKind::kPeriodicEdges;
  predicate.input_id = input;
  predicate.periodic = {minimum, maximum, edges, window, gap};
  return predicate;
}

RuleConfig rule(RuleId id, RuleOutputKind kind, PredicateConfig predicate) {
  RuleConfig result;
  result.id = id;
  result.enabled = true;
  result.output.kind = kind;
  result.group_count = 1;
  result.groups[0].predicate_count = 1;
  result.groups[0].predicates[0] = predicate;
  return result;
}

CompiledDecoder compile_ok(const DecoderConfig& config) {
  CompiledDecoder compiled;
  CompileError error;
  expect(compile(config, &compiled, &error), "configuration must compile");
  return compiled;
}

void test_stable_deadline_and_generation() {
  DecoderConfig config;
  config.input_ids[0] = 4;
  config.input_count = 1;
  config.rules[0] = rule(1, RuleOutputKind::kPosition, stable(4, true, 2500));
  config.rules[0].output.position = PositionValue::kOpened;
  config.rule_count = 1;
  const auto compiled = compile_ok(config);
  SignalDecoder decoder(compiled);

  expect(decoder.initialize(4, true, 100), "input must initialize");
  expect(!decoder.result().position_valid,
         "stable endpoint must not match before hold duration");
  expect(decoder.next_deadline().valid && decoder.next_deadline().at == 2600,
         "stable endpoint must schedule exact hold deadline");
  const auto generation = decoder.result().generation;
  decoder.advance(2599);
  expect(decoder.result().generation == generation,
         "unchanged evidence must not increment generation");
  decoder.advance(2600);
  expect(decoder.result().position_valid &&
             decoder.result().position == PositionValue::kOpened,
         "stable endpoint must match at inclusive deadline");
  expect(!decoder.next_deadline().valid,
         "settled stable endpoint must have no deadline");
}

DecoderConfig ducati_config() {
  DecoderConfig config;
  config.input_ids[0] = 4;
  config.input_count = 1;

  config.rules[0] = rule(1, RuleOutputKind::kPosition, stable(4, true, 2500));
  config.rules[0].output.position = PositionValue::kOpened;
  config.rules[1] = rule(2, RuleOutputKind::kPosition, stable(4, false, 2500));
  config.rules[1].output.position = PositionValue::kClosed;

  config.rules[2] =
      rule(3, RuleOutputKind::kMovement, periodic(4, 800, 1200, 3, 3500, 1500));
  config.rules[2].output.movement = MovementValue::kOpening;
  config.rules[2].movement.match_age_limit_ms = 5000;
  config.rules[2].movement.expiry = MatchAgeExpiry::kUnknownAndObstructed;

  config.rules[3] =
      rule(4, RuleOutputKind::kMovement, periodic(4, 350, 650, 4, 3000, 900));
  config.rules[3].output.movement = MovementValue::kClosing;
  config.rule_count = 4;
  return config;
}

void test_one_gpio_independent_rules_and_endpoint_authority() {
  const auto compiled = compile_ok(ducati_config());
  SignalDecoder decoder(compiled);
  decoder.initialize(4, false, 0);
  decoder.update(4, true, 1000);
  decoder.update(4, false, 2000);
  decoder.update(4, true, 3000);
  expect(decoder.result().movement_valid &&
             decoder.result().movement == MovementValue::kOpening,
         "three slow edges on shared GPIO must independently mean OPENING");
  expect(decoder.next_deadline().valid && decoder.next_deadline().at == 4500,
         "periodic match must schedule missing-edge deadline");

  decoder.advance(4500);
  expect(!decoder.result().movement_valid,
         "missing-edge boundary must withdraw periodic evidence");
  expect(decoder.next_deadline().valid && decoder.next_deadline().at == 5500,
         "independent solid-high rule must keep its own hold deadline");
  decoder.advance(5500);
  expect(decoder.result().position_valid &&
             decoder.result().position == PositionValue::kOpened,
         "same GPIO solid-high rule must independently prove OPENED");
}

void test_match_age_obstruction_and_endpoint_clear() {
  auto config = ducati_config();
  config.rules[2].movement.match_age_limit_ms = 2000;
  const auto compiled = compile_ok(config);
  SignalDecoder decoder(compiled);
  decoder.initialize(4, false, 0);
  decoder.update(4, true, 1000);
  decoder.update(4, false, 2000);
  decoder.update(4, true, 3000);
  decoder.update(4, false, 4000);
  decoder.update(4, true, 5000);
  expect(!decoder.result().movement_valid && decoder.result().obstructed &&
             decoder.result().movement_withdrawn_by_age,
         "aged movement must become unknown plus obstructed exactly at limit");

  decoder.advance(7500);
  expect(decoder.result().position_valid && !decoder.result().obstructed,
         "independent endpoint proof must clear public rule obstruction");
}

void test_channel_ambiguity() {
  DecoderConfig config;
  config.input_ids[0] = 1;
  config.input_count = 1;
  config.rules[0] = rule(1, RuleOutputKind::kPosition, stable(1, true, 10));
  config.rules[0].output.position = PositionValue::kOpened;
  config.rules[1] = rule(2, RuleOutputKind::kPosition, stable(1, true, 20));
  config.rules[1].output.position = PositionValue::kClosed;
  config.rule_count = 2;
  const auto compiled = compile_ok(config);
  SignalDecoder decoder(compiled);
  decoder.initialize(1, true, 0);
  decoder.advance(20);
  expect(decoder.result().health == DecoderHealth::kAmbiguousPosition &&
             !decoder.result().position_valid,
         "conflicting endpoint rules must report position ambiguity");
}

void test_movement_ambiguity_and_shared_fault_rule() {
  DecoderConfig config;
  config.input_ids[0] = 4;
  config.input_count = 1;
  config.rules[0] =
      rule(1, RuleOutputKind::kMovement, periodic(4, 400, 600, 3, 2000, 800));
  config.rules[0].output.movement = MovementValue::kOpening;
  config.rules[1] =
      rule(2, RuleOutputKind::kMovement, periodic(4, 450, 650, 3, 2000, 800));
  config.rules[1].output.movement = MovementValue::kClosing;
  config.rules[2] = rule(3, RuleOutputKind::kFault, stable(4, false, 1000));
  config.rules[2].output.fault = FaultValue::kObstructed;
  config.rule_count = 3;
  const auto compiled = compile_ok(config);
  SignalDecoder decoder(compiled);
  decoder.initialize(4, false, 0);
  decoder.update(4, true, 500);
  decoder.update(4, false, 1000);
  decoder.update(4, true, 1500);
  expect(decoder.result().health == DecoderHealth::kAmbiguousMovement &&
             !decoder.result().movement_valid,
         "overlapping movement rules must report movement ambiguity");
  decoder.update(4, false, 2000);
  decoder.advance(3000);
  expect(decoder.result().obstructed,
         "same GPIO becoming solid low must independently assert fault rule");
}

void test_hard_validation() {
  DecoderConfig config;
  config.input_ids[0] = 4;
  config.input_count = 1;
  config.rules[0] =
      rule(1, RuleOutputKind::kMovement, periodic(4, 1000, 500, 3, 3000, 1200));
  config.rule_count = 1;
  CompiledDecoder compiled;
  CompileError error;
  expect(!compile(config, &compiled, &error) &&
             error.code == CompileErrorCode::kInvalidPredicate,
         "invalid periodic range must be rejected");

  config.rules[0] = rule(1, RuleOutputKind::kPosition, stable(9, true, 100));
  expect(!compile(config, &compiled, &error) &&
             error.code == CompileErrorCode::kUnknownInputId,
         "unknown predicate input must be rejected");
}

void test_wrap_safe_stable_deadline() {
  DecoderConfig config;
  config.input_ids[0] = 4;
  config.input_count = 1;
  config.rules[0] = rule(1, RuleOutputKind::kPosition, stable(4, true, 40));
  config.rule_count = 1;
  const auto compiled = compile_ok(config);
  SignalDecoder decoder(compiled);
  constexpr Tick start = 0xfffffff0U;
  decoder.initialize(4, true, start);
  expect(decoder.next_deadline().valid && decoder.next_deadline().at == 24,
         "deadline must wrap modulo 32-bit time");
  decoder.advance(24);
  expect(decoder.result().position_valid,
         "stable duration must evaluate correctly across wrap");
}

void test_timestamp_order_and_idempotent_updates() {
  DecoderConfig config;
  config.input_ids[0] = 4;
  config.input_count = 1;
  config.rules[0] = rule(1, RuleOutputKind::kPosition, stable(4, true, 100));
  config.rule_count = 1;
  const auto compiled = compile_ok(config);
  SignalDecoder decoder(compiled);
  decoder.initialize(4, true, 1000);
  expect(!decoder.update(4, false, 999),
         "out-of-order input event must be rejected");
  expect(decoder.update(4, true, 1050),
         "same-level update must be accepted without creating an edge");
  decoder.advance(1100);
  expect(decoder.result().position_valid,
         "same-level update must not reset stable hold age");
  const auto generation = decoder.result().generation;
  decoder.advance(1099);
  expect(decoder.result().generation == generation,
         "out-of-order advance must be ignored");
}

void test_and_or_groups_and_monitoring_health() {
  DecoderConfig config;
  config.input_ids[0] = 1;
  config.input_ids[1] = 2;
  config.input_count = 2;
  auto& opened = config.rules[0];
  opened.id = 1;
  opened.enabled = true;
  opened.output.kind = RuleOutputKind::kPosition;
  opened.output.position = PositionValue::kOpened;
  opened.group_count = 2;
  opened.groups[0].predicate_count = 2;
  opened.groups[0].predicates[0] = stable(1, true, 10);
  opened.groups[0].predicates[1] = stable(2, false, 10);
  opened.groups[1].predicate_count = 1;
  opened.groups[1].predicates[0] = stable(2, true, 20);
  config.rule_count = 1;
  const auto compiled = compile_ok(config);
  SignalDecoder decoder(compiled);
  decoder.initialize(1, true, 0);
  decoder.initialize(2, false, 0);
  decoder.advance(10);
  expect(decoder.result().position_valid,
         "ALL predicates in one satisfied ANY group must assert rule");
  decoder.set_monitoring_healthy(false, 11);
  expect(decoder.result().health == DecoderHealth::kMonitoringFailed &&
             !decoder.result().position_valid,
         "monitoring failure must withhold authoritative position");
  decoder.set_monitoring_healthy(true, 12);
  expect(decoder.result().position_valid,
         "monitoring recovery must re-evaluate retained independent evidence");
}

}  // namespace

int main() {
  test_stable_deadline_and_generation();
  test_one_gpio_independent_rules_and_endpoint_authority();
  test_match_age_obstruction_and_endpoint_clear();
  test_channel_ambiguity();
  test_movement_ambiguity_and_shared_fault_rule();
  test_hard_validation();
  test_wrap_safe_stable_deadline();
  test_timestamp_order_and_idempotent_updates();
  test_and_or_groups_and_monitoring_health();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "All signal decoder tests passed\n";
  return EXIT_SUCCESS;
}
