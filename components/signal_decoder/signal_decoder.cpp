#include "signal_decoder.hpp"

#include <algorithm>
#include <type_traits>

namespace gate::signal_decoder {
namespace {

bool elapsed_at_least(Tick now, Tick since, Tick duration) {
  return static_cast<Tick>(now - since) >= duration;
}

bool is_future(Tick now, Tick candidate) {
  return static_cast<std::int32_t>(candidate - now) > 0;
}

void consider_deadline(Tick now, Tick candidate, Deadline* deadline) {
  if (!is_future(now, candidate)) return;
  if (!deadline->valid ||
      static_cast<std::int32_t>(candidate - deadline->at) < 0) {
    deadline->valid = true;
    deadline->at = candidate;
  }
}

template <typename Enum>
bool enum_at_most(Enum value, Enum maximum) {
  using Underlying = typename std::underlying_type<Enum>::type;
  return static_cast<Underlying>(value) <= static_cast<Underlying>(maximum);
}

void set_error(CompileError* error, CompileErrorCode code, RuleId rule_id = 0,
               std::uint8_t group = 0, std::uint8_t predicate = 0) {
  if (error == nullptr) return;
  *error = {code, rule_id, group, predicate};
}

int find_source_input(const DecoderConfig& source, InputId id) {
  for (std::uint8_t i = 0; i < source.input_count; ++i) {
    if (source.input_ids[i] == id) return i;
  }
  return -1;
}

bool lifecycle_empty(const MovementLifecycleConfig& lifecycle) {
  return lifecycle.entry_confirmation_ms == 0 &&
         lifecycle.loss_confirmation_ms == 0 &&
         lifecycle.match_age_limit_ms == 0 &&
         lifecycle.expiry == MatchAgeExpiry::kNone;
}

bool predicate_config_equal(const PredicateConfig& left,
                            const PredicateConfig& right) {
  if (left.kind != right.kind || left.input_id != right.input_id) return false;
  if (left.kind == PredicateKind::kStableLevel) {
    return left.stable.level == right.stable.level &&
           left.stable.hold_ms == right.stable.hold_ms;
  }
  return left.periodic.minimum_interval_ms ==
             right.periodic.minimum_interval_ms &&
         left.periodic.maximum_interval_ms ==
             right.periodic.maximum_interval_ms &&
         left.periodic.minimum_edges == right.periodic.minimum_edges &&
         left.periodic.observation_window_ms ==
             right.periodic.observation_window_ms &&
         left.periodic.maximum_gap_ms == right.periodic.maximum_gap_ms;
}

bool expression_equal(const RuleConfig& left, const RuleConfig& right) {
  if (left.group_count > DecoderLimits::kMaxGroupsPerRule ||
      right.group_count > DecoderLimits::kMaxGroupsPerRule) {
    return false;
  }
  if (left.group_count != right.group_count) return false;
  for (std::uint8_t g = 0; g < left.group_count; ++g) {
    if (left.groups[g].predicate_count >
            DecoderLimits::kMaxPredicatesPerGroup ||
        right.groups[g].predicate_count >
            DecoderLimits::kMaxPredicatesPerGroup) {
      return false;
    }
    if (left.groups[g].predicate_count != right.groups[g].predicate_count) {
      return false;
    }
    for (std::uint8_t p = 0; p < left.groups[g].predicate_count; ++p) {
      if (!predicate_config_equal(left.groups[g].predicates[p],
                                  right.groups[g].predicates[p])) {
        return false;
      }
    }
  }
  return true;
}

bool conflicting_output(const RuleOutputConfig& left,
                        const RuleOutputConfig& right) {
  if (left.kind != right.kind) return false;
  switch (left.kind) {
    case RuleOutputKind::kPosition: return left.position != right.position;
    case RuleOutputKind::kMovement: return left.movement != right.movement;
    case RuleOutputKind::kFault: return left.fault != right.fault;
  }
  return false;
}

Tick edge_at(const EdgeHistory& history, std::uint8_t logical_index) {
  const std::size_t capacity = DecoderLimits::kMaxEdgesPerInput;
  const std::size_t oldest =
      (static_cast<std::size_t>(history.head) + capacity - history.count) %
      capacity;
  return history.timestamps[(oldest + logical_index) % capacity];
}

void push_edge(EdgeHistory* history, Tick at) {
  history->timestamps[history->head] = at;
  history->head = static_cast<std::uint8_t>(
      (history->head + 1U) % DecoderLimits::kMaxEdgesPerInput);
  if (history->count < DecoderLimits::kMaxEdgesPerInput) ++history->count;
}

struct PredicateEvaluation {
  bool value{false};
  PredicateDiagnostic diagnostic{};
  Deadline deadline{};
};

PredicateEvaluation evaluate_predicate(const CompiledPredicate& predicate,
                                       const InputState& input, Tick now) {
  PredicateEvaluation evaluation;
  if (!input.valid) return evaluation;
  evaluation.diagnostic.evidence_valid = true;

  if (predicate.kind == PredicateKind::kStableLevel) {
    if (input.level != predicate.stable.level) return evaluation;
    const Tick age = now - input.level_since;
    evaluation.diagnostic.evidence_age_ms = age;
    evaluation.value = age >= predicate.stable.hold_ms;
    evaluation.diagnostic.value = evaluation.value;
    if (!evaluation.value) {
      consider_deadline(now, input.level_since + predicate.stable.hold_ms,
                        &evaluation.deadline);
    }
    return evaluation;
  }

  const auto& config = predicate.periodic;
  const auto& history = input.edges;
  if (history.count == 0) return evaluation;

  const Tick newest = edge_at(history, history.count - 1U);
  evaluation.diagnostic.evidence_age_ms = now - newest;
  if (history.count >= 2) {
    evaluation.diagnostic.latest_interval_ms =
        newest - edge_at(history, history.count - 2U);
  }

  std::uint8_t run_edges = 1;
  Tick run_oldest = newest;
  for (std::uint8_t offset = 1; offset < history.count; ++offset) {
    const std::uint8_t newer_index = history.count - offset;
    const std::uint8_t older_index = newer_index - 1U;
    const Tick newer = edge_at(history, newer_index);
    const Tick older = edge_at(history, older_index);
    const Tick interval = newer - older;
    if (interval < config.minimum_interval_ms ||
        interval > config.maximum_interval_ms) {
      break;
    }
    if (now - older > config.observation_window_ms) break;
    run_oldest = older;
    ++run_edges;
  }
  evaluation.diagnostic.qualifying_edge_count = run_edges;
  const bool enough_edges = run_edges >= config.minimum_edges;
  const bool inside_window = now - run_oldest <= config.observation_window_ms;
  const bool gap_valid = now - newest < config.maximum_gap_ms;
  evaluation.value = enough_edges && inside_window && gap_valid;
  evaluation.diagnostic.value = evaluation.value;
  if (evaluation.value) {
    consider_deadline(now, newest + config.maximum_gap_ms,
                      &evaluation.deadline);
    // The oldest edge stops qualifying one millisecond after the inclusive
    // observation-window boundary.
    consider_deadline(now, run_oldest + config.observation_window_ms + 1U,
                      &evaluation.deadline);
  }
  return evaluation;
}

struct RuleExpressionEvaluation {
  bool value{false};
  Deadline deadline{};
};

RuleExpressionEvaluation evaluate_expression(const CompiledRule& rule,
                                             const DecoderState& state,
                                             Tick now) {
  RuleExpressionEvaluation result;
  for (std::uint8_t g = 0; g < rule.group_count; ++g) {
    bool group_value = true;
    for (std::uint8_t p = 0; p < rule.groups[g].predicate_count; ++p) {
      const auto& predicate = rule.groups[g].predicates[p];
      const auto evaluation =
          evaluate_predicate(predicate, state.inputs[predicate.input], now);
      group_value = group_value && evaluation.value;
      if (evaluation.deadline.valid) {
        consider_deadline(now, evaluation.deadline.at, &result.deadline);
      }
    }
    result.value = result.value || group_value;
  }
  return result;
}

bool result_equal(const DecoderResult& left, const DecoderResult& right) {
  return left.position_valid == right.position_valid &&
         left.position == right.position &&
         left.movement_valid == right.movement_valid &&
         left.movement == right.movement &&
         left.obstructed == right.obstructed &&
         left.movement_withdrawn_by_age ==
             right.movement_withdrawn_by_age &&
         left.health == right.health;
}

}  // namespace

bool compile(const DecoderConfig& source, CompiledDecoder* destination,
             CompileError* error) {
  if (destination == nullptr) {
    set_error(error, CompileErrorCode::kInvalidCount);
    return false;
  }
  set_error(error, CompileErrorCode::kNone);
  if (source.input_count == 0 ||
      source.input_count > DecoderLimits::kMaxInputs ||
      source.rule_count > DecoderLimits::kMaxRules) {
    set_error(error, CompileErrorCode::kInvalidCount);
    return false;
  }
  for (std::uint8_t i = 0; i < source.input_count; ++i) {
    for (std::uint8_t j = i + 1U; j < source.input_count; ++j) {
      if (source.input_ids[i] == source.input_ids[j]) {
        set_error(error, CompileErrorCode::kDuplicateInputId);
        return false;
      }
    }
  }

  for (std::uint8_t i = 0; i < source.rule_count; ++i) {
    if (!source.rules[i].enabled) continue;
    for (std::uint8_t j = i + 1U; j < source.rule_count; ++j) {
      if (source.rules[j].enabled &&
          source.rules[i].id == source.rules[j].id) {
        set_error(error, CompileErrorCode::kDuplicateRuleId,
                  source.rules[i].id);
        return false;
      }
      if (source.rules[j].enabled &&
          expression_equal(source.rules[i], source.rules[j]) &&
          conflicting_output(source.rules[i].output,
                             source.rules[j].output)) {
        set_error(error, CompileErrorCode::kConflictingExpression,
                  source.rules[j].id);
        return false;
      }
    }
  }

  CompiledDecoder compiled;
  compiled.input_count = source.input_count;
  for (std::uint8_t i = 0; i < source.input_count; ++i) {
    compiled.input_ids[i] = source.input_ids[i];
  }

  for (std::uint8_t source_index = 0; source_index < source.rule_count;
       ++source_index) {
    const RuleConfig& rule = source.rules[source_index];
    if (!rule.enabled) continue;
    if (!enum_at_most(rule.output.kind, RuleOutputKind::kFault) ||
        !enum_at_most(rule.output.position, PositionValue::kClosed) ||
        !enum_at_most(rule.output.movement, MovementValue::kStopped) ||
        !enum_at_most(rule.output.fault, FaultValue::kObstructed) ||
        rule.group_count == 0 ||
        rule.group_count > DecoderLimits::kMaxGroupsPerRule) {
      set_error(error, rule.group_count == 0 ? CompileErrorCode::kEmptyRule
                                             : CompileErrorCode::kInvalidCount,
                rule.id);
      return false;
    }
    const bool movement = rule.output.kind == RuleOutputKind::kMovement;
    const auto& lifecycle = rule.movement;
    if ((!movement && !lifecycle_empty(lifecycle)) ||
        !enum_at_most(lifecycle.expiry,
                      MatchAgeExpiry::kUnknownAndObstructed) ||
        lifecycle.entry_confirmation_ms > DecoderLimits::kMaxDurationMs ||
        lifecycle.loss_confirmation_ms > DecoderLimits::kMaxDurationMs ||
        lifecycle.match_age_limit_ms > DecoderLimits::kMaxDurationMs ||
        ((lifecycle.match_age_limit_ms == 0) !=
         (lifecycle.expiry == MatchAgeExpiry::kNone))) {
      set_error(error, CompileErrorCode::kInvalidLifecycle, rule.id);
      return false;
    }

    CompiledRule output;
    output.id = rule.id;
    output.output = rule.output;
    output.group_count = rule.group_count;
    output.movement = rule.movement;
    for (std::uint8_t g = 0; g < rule.group_count; ++g) {
      const auto& group = rule.groups[g];
      if (group.predicate_count == 0) {
        set_error(error, CompileErrorCode::kEmptyGroup, rule.id, g);
        return false;
      }
      if (group.predicate_count > DecoderLimits::kMaxPredicatesPerGroup) {
        set_error(error, CompileErrorCode::kInvalidCount, rule.id, g);
        return false;
      }
      output.groups[g].predicate_count = group.predicate_count;
      for (std::uint8_t p = 0; p < group.predicate_count; ++p) {
        const PredicateConfig& predicate = group.predicates[p];
        const int input = find_source_input(source, predicate.input_id);
        if (input < 0) {
          set_error(error, CompileErrorCode::kUnknownInputId, rule.id, g, p);
          return false;
        }
        if (!enum_at_most(predicate.kind, PredicateKind::kPeriodicEdges)) {
          set_error(error, CompileErrorCode::kInvalidPredicate, rule.id, g, p);
          return false;
        }
        if (predicate.kind == PredicateKind::kStableLevel) {
          if (predicate.stable.hold_ms == 0 ||
              predicate.stable.hold_ms > DecoderLimits::kMaxDurationMs) {
            set_error(error, CompileErrorCode::kInvalidPredicate, rule.id, g,
                      p);
            return false;
          }
        } else {
          const auto& periodic = predicate.periodic;
          const std::uint64_t minimum_span =
              static_cast<std::uint64_t>(periodic.minimum_edges - 1U) *
              periodic.minimum_interval_ms;
          if (periodic.minimum_interval_ms == 0 ||
              periodic.minimum_interval_ms > periodic.maximum_interval_ms ||
              periodic.maximum_interval_ms > DecoderLimits::kMaxDurationMs ||
              periodic.minimum_edges < 2 ||
              periodic.minimum_edges > DecoderLimits::kMaxEdgesPerInput ||
              periodic.observation_window_ms == 0 ||
              periodic.observation_window_ms > DecoderLimits::kMaxDurationMs ||
              periodic.maximum_gap_ms < periodic.maximum_interval_ms ||
              periodic.maximum_gap_ms > DecoderLimits::kMaxDurationMs ||
              minimum_span > periodic.observation_window_ms) {
            set_error(error, CompileErrorCode::kInvalidPredicate, rule.id, g,
                      p);
            return false;
          }
          compiled.required_edge_capacity[input] = std::max(
              compiled.required_edge_capacity[input], periodic.minimum_edges);
          if (compiled.required_edge_capacity[input] >
              DecoderLimits::kMaxEdgesPerInput) {
            set_error(error, CompileErrorCode::kHistoryCapacityExceeded,
                      rule.id, g, p);
            return false;
          }
        }
        output.groups[g].predicates[p] = {
            predicate.kind, static_cast<InputIndex>(input), predicate.stable,
            predicate.periodic};
      }
    }

    const std::uint8_t compiled_index = compiled.rule_count++;
    compiled.rules[compiled_index] = output;
    switch (rule.output.kind) {
      case RuleOutputKind::kPosition:
        compiled.position_rule_indexes[compiled.position_rule_count++] =
            compiled_index;
        break;
      case RuleOutputKind::kMovement:
        compiled.movement_rule_indexes[compiled.movement_rule_count++] =
            compiled_index;
        break;
      case RuleOutputKind::kFault:
        compiled.fault_rule_indexes[compiled.fault_rule_count++] =
            compiled_index;
        break;
    }
  }

  *destination = compiled;
  return true;
}

SignalDecoder::SignalDecoder(const CompiledDecoder& config) : config_(config) {}

bool SignalDecoder::initialize(InputId input_id, bool level, Tick now) {
  const int index = find_input(input_id);
  if (index < 0 || state_.inputs[index].valid || !accept_time(now)) return false;
  state_.inputs[index].valid = true;
  state_.inputs[index].level = level;
  state_.inputs[index].level_since = now;
  state_.inputs[index].last_edge_at = now;
  evaluate(now);
  return true;
}

bool SignalDecoder::update(InputId input_id, bool level, Tick now) {
  const int index = find_input(input_id);
  if (index < 0 || !state_.inputs[index].valid || !accept_time(now)) return false;
  InputState& input = state_.inputs[index];
  if (input.level != level) {
    input.level = level;
    input.level_since = now;
    input.last_edge_at = now;
    push_edge(&input.edges, now);
  }
  evaluate(now);
  return true;
}

void SignalDecoder::set_monitoring_healthy(bool healthy, Tick now) {
  if (!accept_time(now)) return;
  state_.monitoring_healthy = healthy;
  evaluate(now);
}

void SignalDecoder::advance(Tick now) {
  if (!accept_time(now)) return;
  evaluate(now);
}

const DecoderResult& SignalDecoder::result() const { return state_.result; }
Deadline SignalDecoder::next_deadline() const { return next_deadline_; }

DecoderDiagnostics SignalDecoder::diagnostics(Tick now) const {
  DecoderDiagnostics diagnostics;
  for (std::uint8_t r = 0; r < config_.rule_count; ++r) {
    const CompiledRule& rule = config_.rules[r];
    RuleDiagnostic& rule_diagnostic = diagnostics.rules[diagnostics.rule_count++];
    rule_diagnostic.id = rule.id;
    bool expression = false;
    for (std::uint8_t g = 0; g < rule.group_count; ++g) {
      bool group = true;
      for (std::uint8_t p = 0; p < rule.groups[g].predicate_count; ++p) {
        const auto& predicate = rule.groups[g].predicates[p];
        const auto evaluation =
            evaluate_predicate(predicate, state_.inputs[predicate.input], now);
        diagnostics.predicates[diagnostics.predicate_count++] =
            evaluation.diagnostic;
        group = group && evaluation.value;
      }
      expression = expression || group;
    }
    rule_diagnostic.expression_value = expression;
    if (rule.output.kind == RuleOutputKind::kMovement) {
      const auto& movement = state_.movement_rules[r];
      rule_diagnostic.movement_phase = movement.phase;
      if (movement.phase == MovementRulePhase::kMatched ||
          movement.phase == MovementRulePhase::kLossPending ||
          movement.phase == MovementRulePhase::kExpired) {
        rule_diagnostic.match_age_ms = now - movement.matched_since;
      }
      rule_diagnostic.output_asserted =
          movement.phase == MovementRulePhase::kMatched ||
          movement.phase == MovementRulePhase::kLossPending;
    } else {
      rule_diagnostic.output_asserted = expression;
    }
  }
  return diagnostics;
}

void SignalDecoder::evaluate(Tick now) {
  next_deadline_ = {};

  std::array<bool, DecoderLimits::kMaxRules> expressions{};
  for (std::uint8_t r = 0; r < config_.rule_count; ++r) {
    const auto expression = evaluate_expression(config_.rules[r], state_, now);
    expressions[r] = expression.value;
    if (expression.deadline.valid) {
      consider_deadline(now, expression.deadline.at, &next_deadline_);
    }
  }

  // Zero-duration lifecycle transitions are consumed in this call. A fixed
  // bound is sufficient because each pass must advance or settle a phase.
  for (std::uint8_t pass = 0; pass < 4; ++pass) {
    bool changed = false;
    for (std::uint8_t i = 0; i < config_.movement_rule_count; ++i) {
      const std::uint8_t rule_index = config_.movement_rule_indexes[i];
      const CompiledRule& rule = config_.rules[rule_index];
      MovementRuleState& lifecycle = state_.movement_rules[rule_index];
      const bool matched = expressions[rule_index];
      switch (lifecycle.phase) {
        case MovementRulePhase::kUnmatched:
          if (matched) {
            lifecycle.phase = rule.movement.entry_confirmation_ms == 0
                                  ? MovementRulePhase::kMatched
                                  : MovementRulePhase::kEntryPending;
            lifecycle.phase_since = now;
            if (lifecycle.phase == MovementRulePhase::kMatched) {
              lifecycle.matched_since = now;
            }
            changed = true;
          }
          break;
        case MovementRulePhase::kEntryPending:
          if (!matched) {
            lifecycle.phase = MovementRulePhase::kUnmatched;
            lifecycle.phase_since = now;
            changed = true;
          } else if (elapsed_at_least(now, lifecycle.phase_since,
                                     rule.movement.entry_confirmation_ms)) {
            lifecycle.phase = MovementRulePhase::kMatched;
            lifecycle.phase_since = now;
            lifecycle.matched_since = now;
            changed = true;
          }
          break;
        case MovementRulePhase::kMatched:
          if (rule.movement.match_age_limit_ms != 0 &&
              elapsed_at_least(now, lifecycle.matched_since,
                               rule.movement.match_age_limit_ms)) {
            lifecycle.phase = MovementRulePhase::kExpired;
            lifecycle.phase_since = now;
            changed = true;
          } else if (!matched) {
            lifecycle.phase = rule.movement.loss_confirmation_ms == 0
                                  ? MovementRulePhase::kUnmatched
                                  : MovementRulePhase::kLossPending;
            lifecycle.phase_since = now;
            changed = true;
          }
          break;
        case MovementRulePhase::kLossPending:
          if (matched) {
            lifecycle.phase = MovementRulePhase::kMatched;
            lifecycle.phase_since = now;
            changed = true;
          } else if (elapsed_at_least(now, lifecycle.phase_since,
                                     rule.movement.loss_confirmation_ms)) {
            lifecycle.phase = MovementRulePhase::kUnmatched;
            lifecycle.phase_since = now;
            lifecycle.matched_since = 0;
            changed = true;
          }
          break;
        case MovementRulePhase::kExpired:
          if (!matched) {
            lifecycle.phase = rule.movement.loss_confirmation_ms == 0
                                  ? MovementRulePhase::kUnmatched
                                  : MovementRulePhase::kLossPending;
            lifecycle.phase_since = now;
            changed = true;
          }
          break;
      }
    }
    if (!changed) break;
  }

  for (std::uint8_t i = 0; i < config_.movement_rule_count; ++i) {
    const std::uint8_t rule_index = config_.movement_rule_indexes[i];
    const auto& rule = config_.rules[rule_index];
    const auto& lifecycle = state_.movement_rules[rule_index];
    switch (lifecycle.phase) {
      case MovementRulePhase::kEntryPending:
        consider_deadline(now,
                          lifecycle.phase_since +
                              rule.movement.entry_confirmation_ms,
                          &next_deadline_);
        break;
      case MovementRulePhase::kMatched:
      case MovementRulePhase::kLossPending:
        if (rule.movement.match_age_limit_ms != 0) {
          consider_deadline(now,
                            lifecycle.matched_since +
                                rule.movement.match_age_limit_ms,
                            &next_deadline_);
        }
        if (lifecycle.phase == MovementRulePhase::kLossPending) {
          consider_deadline(now,
                            lifecycle.phase_since +
                                rule.movement.loss_confirmation_ms,
                            &next_deadline_);
        }
        break;
      case MovementRulePhase::kExpired:
        if (!expressions[rule_index] &&
            rule.movement.loss_confirmation_ms != 0) {
          consider_deadline(now,
                            lifecycle.phase_since +
                                rule.movement.loss_confirmation_ms,
                            &next_deadline_);
        }
        break;
      case MovementRulePhase::kUnmatched: break;
    }
  }

  DecoderResult next;
  next.generation = state_.result.generation;
  bool all_inputs_valid = config_.input_count > 0;
  for (std::uint8_t i = 0; i < config_.input_count; ++i) {
    all_inputs_valid = all_inputs_valid && state_.inputs[i].valid;
  }

  bool position_seen = false;
  bool position_ambiguous = false;
  for (std::uint8_t i = 0; i < config_.position_rule_count; ++i) {
    const std::uint8_t rule_index = config_.position_rule_indexes[i];
    if (!expressions[rule_index]) continue;
    const PositionValue value = config_.rules[rule_index].output.position;
    if (position_seen && next.position != value) position_ambiguous = true;
    next.position = value;
    position_seen = true;
  }
  next.position_valid = position_seen && !position_ambiguous;

  bool movement_seen = false;
  bool movement_ambiguous = false;
  bool age_obstruction = false;
  for (std::uint8_t i = 0; i < config_.movement_rule_count; ++i) {
    const std::uint8_t rule_index = config_.movement_rule_indexes[i];
    const auto& rule = config_.rules[rule_index];
    const auto phase = state_.movement_rules[rule_index].phase;
    const bool asserted = phase == MovementRulePhase::kMatched ||
                          phase == MovementRulePhase::kLossPending;
    if (asserted) {
      const MovementValue value = rule.output.movement;
      if (movement_seen && next.movement != value) movement_ambiguous = true;
      next.movement = value;
      movement_seen = true;
    } else if (phase == MovementRulePhase::kExpired) {
      next.movement_withdrawn_by_age = true;
      age_obstruction =
          age_obstruction ||
          rule.movement.expiry == MatchAgeExpiry::kObstructed ||
          rule.movement.expiry == MatchAgeExpiry::kUnknownAndObstructed;
    }
  }
  next.movement_valid = movement_seen && !movement_ambiguous &&
                        !next.position_valid;

  bool fault_obstruction = false;
  for (std::uint8_t i = 0; i < config_.fault_rule_count; ++i) {
    const std::uint8_t rule_index = config_.fault_rule_indexes[i];
    if (expressions[rule_index] &&
        config_.rules[rule_index].output.fault == FaultValue::kObstructed) {
      fault_obstruction = true;
    }
  }
  next.obstructed = !next.position_valid &&
                    (fault_obstruction || age_obstruction);

  if (!state_.monitoring_healthy) {
    next.position_valid = false;
    next.movement_valid = false;
    next.health = DecoderHealth::kMonitoringFailed;
  } else if (position_ambiguous) {
    next.position_valid = false;
    next.movement_valid = false;
    next.health = DecoderHealth::kAmbiguousPosition;
  } else if (movement_ambiguous) {
    next.movement_valid = false;
    next.health = DecoderHealth::kAmbiguousMovement;
  } else if (!all_inputs_valid) {
    next.health = DecoderHealth::kGatheringEvidence;
  } else {
    next.health = DecoderHealth::kHealthy;
  }

  if (!result_equal(state_.result, next)) ++next.generation;
  state_.result = next;
}

int SignalDecoder::find_input(InputId input_id) const {
  for (std::uint8_t i = 0; i < config_.input_count; ++i) {
    if (config_.input_ids[i] == input_id) return i;
  }
  return -1;
}

bool SignalDecoder::accept_time(Tick now) {
  if (time_valid_ && static_cast<std::int32_t>(now - last_now_) < 0) {
    return false;
  }
  time_valid_ = true;
  last_now_ = now;
  return true;
}

}  // namespace gate::signal_decoder
