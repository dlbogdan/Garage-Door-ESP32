#pragma once

#include <cstdint>

namespace gate::controller {

enum class State : std::uint8_t {
  kClosed,
  kOpening,
  kOpen,
  kClosing,
  kStoppedOpening,
  kStoppedClosing,
  kUnknownStopped,
};

enum class Target : std::uint8_t { kOpen, kClosed };

enum class EventType : std::uint8_t {
  kBoot,
  kTargetRequested,
  kMaintenancePulseRequested,
  kFeedbackChanged,
  kFeedbackProvedOpen,
  kFeedbackProvedClosed,
  kOpeningTimerExpired,
  kClosingTimerExpired,
  kPulseCompleted,
  kObstructionAcknowledged,
};

enum class CommandResult : std::uint8_t {
  kNotACommand,
  kAccepted,
  kNoChange,
  kRejectedBusy,
};

struct Snapshot {
  State state{State::kUnknownStopped};
  Target target{Target::kOpen};
  bool feedback_active{false};
  bool pulse_active{false};
  bool obstruction{false};
};

struct Event {
  EventType type;
  Target target{Target::kOpen};
  bool feedback_active{false};
};

struct Effects {
  bool start_pulse{false};
  bool start_opening_timer{false};
  bool start_closing_timer{false};
  bool start_feedback_stability_timer{false};
  bool cancel_travel_timers{false};
};

struct Transition {
  Snapshot next;
  Effects effects;
  CommandResult command_result{CommandResult::kNotACommand};
};

Transition reduce(const Snapshot& current, const Event& event);
const char* to_string(State state);
const char* to_string(Target target);

}  // namespace gate::controller
