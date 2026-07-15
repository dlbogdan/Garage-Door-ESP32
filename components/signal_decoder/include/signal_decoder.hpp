#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gate::signal_decoder {

using Tick = std::uint32_t;
using InputId = std::uint8_t;
using RuleId = std::uint8_t;

struct DecoderLimits final {
  static constexpr std::size_t kMaxInputs = 4;
  static constexpr std::size_t kMaxRules = 8;
  static constexpr std::size_t kMaxGroupsPerRule = 2;
  static constexpr std::size_t kMaxPredicatesPerGroup = 3;
  static constexpr std::size_t kMaxEdgesPerInput = 16;
  static constexpr std::size_t kMaxPredicates =
      kMaxRules * kMaxGroupsPerRule * kMaxPredicatesPerGroup;
  static constexpr Tick kMaxDurationMs = 0x7fffffffU;
};

enum class PredicateKind : std::uint8_t {
  kStableLevel,
  kPeriodicEdges,
};

struct StableLevelConfig final {
  bool level{false};
  Tick hold_ms{0};
};

struct PeriodicEdgesConfig final {
  Tick minimum_interval_ms{0};
  Tick maximum_interval_ms{0};
  std::uint8_t minimum_edges{0};
  Tick observation_window_ms{0};
  Tick maximum_gap_ms{0};
};

struct PredicateConfig final {
  PredicateKind kind{PredicateKind::kStableLevel};
  InputId input_id{0};
  StableLevelConfig stable;
  PeriodicEdgesConfig periodic;
};

struct PredicateGroupConfig final {
  std::array<PredicateConfig, DecoderLimits::kMaxPredicatesPerGroup>
      predicates{};
  std::uint8_t predicate_count{0};
};

enum class PositionValue : std::uint8_t { kOpened, kClosed };
enum class MovementValue : std::uint8_t { kOpening, kClosing, kStopped };
enum class FaultValue : std::uint8_t { kObstructed };
enum class RuleOutputKind : std::uint8_t { kPosition, kMovement, kFault };

struct RuleOutputConfig final {
  RuleOutputKind kind{RuleOutputKind::kPosition};
  PositionValue position{PositionValue::kOpened};
  MovementValue movement{MovementValue::kOpening};
  FaultValue fault{FaultValue::kObstructed};
};

enum class MatchAgeExpiry : std::uint8_t {
  kNone,
  kUnknown,
  kObstructed,
  kUnknownAndObstructed,
};

struct MovementLifecycleConfig final {
  Tick entry_confirmation_ms{0};
  Tick loss_confirmation_ms{0};
  Tick match_age_limit_ms{0};
  MatchAgeExpiry expiry{MatchAgeExpiry::kNone};
};

struct RuleConfig final {
  RuleId id{0};
  bool enabled{false};
  RuleOutputConfig output;
  std::array<PredicateGroupConfig, DecoderLimits::kMaxGroupsPerRule> groups{};
  std::uint8_t group_count{0};
  MovementLifecycleConfig movement;
};

struct DecoderConfig final {
  std::array<InputId, DecoderLimits::kMaxInputs> input_ids{};
  std::uint8_t input_count{0};
  std::array<RuleConfig, DecoderLimits::kMaxRules> rules{};
  std::uint8_t rule_count{0};
};

using InputIndex = std::uint8_t;

struct CompiledPredicate final {
  PredicateKind kind{PredicateKind::kStableLevel};
  InputIndex input{0};
  StableLevelConfig stable;
  PeriodicEdgesConfig periodic;
};

struct CompiledPredicateGroup final {
  std::array<CompiledPredicate, DecoderLimits::kMaxPredicatesPerGroup>
      predicates{};
  std::uint8_t predicate_count{0};
};

struct CompiledRule final {
  RuleId id{0};
  RuleOutputConfig output;
  std::array<CompiledPredicateGroup, DecoderLimits::kMaxGroupsPerRule> groups{};
  std::uint8_t group_count{0};
  MovementLifecycleConfig movement;
};

struct CompiledDecoder final {
  std::array<InputId, DecoderLimits::kMaxInputs> input_ids{};
  std::uint8_t input_count{0};
  std::array<CompiledRule, DecoderLimits::kMaxRules> rules{};
  std::uint8_t rule_count{0};
  std::array<std::uint8_t, DecoderLimits::kMaxRules> position_rule_indexes{};
  std::uint8_t position_rule_count{0};
  std::array<std::uint8_t, DecoderLimits::kMaxRules> movement_rule_indexes{};
  std::uint8_t movement_rule_count{0};
  std::array<std::uint8_t, DecoderLimits::kMaxRules> fault_rule_indexes{};
  std::uint8_t fault_rule_count{0};
  std::array<std::uint8_t, DecoderLimits::kMaxInputs>
      required_edge_capacity{};
};

enum class CompileErrorCode : std::uint8_t {
  kNone,
  kInvalidCount,
  kDuplicateInputId,
  kDuplicateRuleId,
  kUnknownInputId,
  kEmptyRule,
  kEmptyGroup,
  kInvalidPredicate,
  kInvalidLifecycle,
  kConflictingExpression,
  kHistoryCapacityExceeded,
};

struct CompileError final {
  CompileErrorCode code{CompileErrorCode::kNone};
  RuleId rule_id{0};
  std::uint8_t group_index{0};
  std::uint8_t predicate_index{0};
};

bool compile(const DecoderConfig& source, CompiledDecoder* destination,
             CompileError* error);

struct Deadline final {
  bool valid{false};
  Tick at{0};
};

struct EdgeHistory final {
  std::array<Tick, DecoderLimits::kMaxEdgesPerInput> timestamps{};
  std::uint8_t head{0};
  std::uint8_t count{0};
};

struct InputState final {
  bool valid{false};
  bool level{false};
  Tick level_since{0};
  Tick last_edge_at{0};
  EdgeHistory edges;
};

enum class MovementRulePhase : std::uint8_t {
  kUnmatched,
  kEntryPending,
  kMatched,
  kLossPending,
  kExpired,
};

struct MovementRuleState final {
  MovementRulePhase phase{MovementRulePhase::kUnmatched};
  Tick phase_since{0};
  Tick matched_since{0};
};

enum class DecoderHealth : std::uint8_t {
  kGatheringEvidence,
  kHealthy,
  kAmbiguousPosition,
  kAmbiguousMovement,
  kMonitoringFailed,
};

struct DecoderResult final {
  bool position_valid{false};
  PositionValue position{PositionValue::kOpened};
  bool movement_valid{false};
  MovementValue movement{MovementValue::kStopped};
  bool obstructed{false};
  bool movement_withdrawn_by_age{false};
  DecoderHealth health{DecoderHealth::kGatheringEvidence};
  std::uint32_t generation{0};
};

struct PredicateDiagnostic final {
  bool value{false};
  bool evidence_valid{false};
  Tick evidence_age_ms{0};
  Tick latest_interval_ms{0};
  std::uint8_t qualifying_edge_count{0};
};

struct RuleDiagnostic final {
  RuleId id{0};
  bool expression_value{false};
  bool output_asserted{false};
  MovementRulePhase movement_phase{MovementRulePhase::kUnmatched};
  Tick match_age_ms{0};
};

struct DecoderDiagnostics final {
  std::array<PredicateDiagnostic, DecoderLimits::kMaxPredicates> predicates{};
  std::uint8_t predicate_count{0};
  std::array<RuleDiagnostic, DecoderLimits::kMaxRules> rules{};
  std::uint8_t rule_count{0};
};

struct DecoderState final {
  std::array<InputState, DecoderLimits::kMaxInputs> inputs{};
  std::array<MovementRuleState, DecoderLimits::kMaxRules> movement_rules{};
  DecoderResult result{};
  bool monitoring_healthy{true};
};

class SignalDecoder final {
 public:
  explicit SignalDecoder(const CompiledDecoder& config);

  bool initialize(InputId input_id, bool level, Tick now);
  bool update(InputId input_id, bool level, Tick now);
  void set_monitoring_healthy(bool healthy, Tick now);
  void advance(Tick now);

  const DecoderResult& result() const;
  Deadline next_deadline() const;
  DecoderDiagnostics diagnostics(Tick now) const;

 private:
  void evaluate(Tick now);
  int find_input(InputId input_id) const;
  bool accept_time(Tick now);

  const CompiledDecoder& config_;
  DecoderState state_{};
  Deadline next_deadline_{};
  Tick last_now_{0};
  bool time_valid_{false};
};

}  // namespace gate::signal_decoder
