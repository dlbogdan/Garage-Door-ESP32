#include "operator_domain.hpp"

#include <cstdlib>
#include <iostream>

using namespace gate::controller;

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void test_single_opened_feedback() {
  expect(normalize_feedback(FeedbackTopology::kSingle, Endpoint::kOpened,
                            {true, false}) == EndpointObservation::kOpened,
         "asserted single OPENED input must prove OPENED");
  expect(normalize_feedback(FeedbackTopology::kSingle, Endpoint::kOpened,
                            {false, false}) == EndpointObservation::kClosed,
         "inactive single OPENED input must prove CLOSED");
}

void test_single_closed_feedback() {
  expect(normalize_feedback(FeedbackTopology::kSingle, Endpoint::kClosed,
                            {false, true}) == EndpointObservation::kClosed,
         "asserted single CLOSED input must prove CLOSED");
  expect(normalize_feedback(FeedbackTopology::kSingle, Endpoint::kClosed,
                            {false, false}) == EndpointObservation::kOpened,
         "inactive single CLOSED input must prove OPENED");
}

void test_dual_feedback_truth_table() {
  expect(normalize_feedback(FeedbackTopology::kDual, Endpoint::kOpened,
                            {false, false}) == EndpointObservation::kBetween,
         "neither dual input must mean BETWEEN");
  expect(normalize_feedback(FeedbackTopology::kDual, Endpoint::kOpened,
                            {true, false}) == EndpointObservation::kOpened,
         "opened-only dual input must mean OPENED");
  expect(normalize_feedback(FeedbackTopology::kDual, Endpoint::kOpened,
                            {false, true}) == EndpointObservation::kClosed,
         "closed-only dual input must mean CLOSED");
  expect(normalize_feedback(FeedbackTopology::kDual, Endpoint::kOpened,
                            {true, true}) ==
             EndpointObservation::kContradictory,
         "both dual inputs must mean CONTRADICTORY");
}

void test_capabilities() {
  const OperatorCapabilities sequential{true, false, false, false};
  expect(supports(sequential, ActuatorCommand::kNone),
         "NONE must always be supported");
  expect(supports(sequential, ActuatorCommand::kStep),
         "sequential capabilities must support STEP");
  expect(!supports(sequential, ActuatorCommand::kOpen),
         "sequential capabilities must reject OPEN");

  const OperatorCapabilities directional{false, true, true, true};
  expect(supports(directional, ActuatorCommand::kOpen) &&
             supports(directional, ActuatorCommand::kClose),
         "directional capabilities must support OPEN and CLOSE");
  expect(!supports(directional, ActuatorCommand::kStep),
         "directional capabilities must reject STEP");
}
}  // namespace

int main() {
  test_single_opened_feedback();
  test_single_closed_feedback();
  test_dual_feedback_truth_table();
  test_capabilities();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "All operator domain tests passed\n";
  return EXIT_SUCCESS;
}
