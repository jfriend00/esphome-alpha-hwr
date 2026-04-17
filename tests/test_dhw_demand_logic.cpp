/**
 * Unit tests for DHW demand pump-on hydraulic voting.
 *
 * These tests mirror the production vote logic to verify that recirculation
 * pump startup transients do not falsely declare DHW demand while genuine
 * open-loop signals remain detectable.
 */

#include <cmath>
#include <iostream>

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message)                                          \
  if (condition) {                                                               \
    tests_passed++;                                                              \
    std::cout << "[PASS] " << message << std::endl;                              \
  } else {                                                                       \
    tests_failed++;                                                              \
    std::cout << "[FAIL] " << message << std::endl;                              \
  }

#define TEST_ASSERT_NEAR(actual, expected, epsilon, message)                     \
  if (std::fabs((actual) - (expected)) <= (epsilon)) {                          \
    tests_passed++;                                                              \
    std::cout << "[PASS] " << message << std::endl;                              \
  } else {                                                                       \
    tests_failed++;                                                              \
    std::cout << "[FAIL] " << message                                            \
              << " (expected: " << (expected) << ", got: " << (actual)          \
              << ")" << std::endl;                                               \
  }

static float deterministic_pump_on_conf(float inlet_deriv, float inlet_psi,
                                        float pump_flow, float current_deriv,
                                        float power_deriv, float head_rate_peak,
                                        bool suppress_transient_votes) {
  constexpr float inlet_pressure_transient_threshold = 0.07f;
  constexpr float inlet_pressure_demand_floor = 5.0f;
  constexpr float pump_flow_collapse_threshold = 0.2f;
  constexpr float motor_current_spike_threshold = 0.001f;
  constexpr float pump_power_spike_threshold = 5.0f;
  constexpr float pump_head_rate_threshold = 3.0f;

  int votes = 0;

  if (!suppress_transient_votes) {
    if (!std::isnan(inlet_deriv) &&
        std::fabs(inlet_deriv) > inlet_pressure_transient_threshold)
      votes++;
  }

  if (!std::isnan(inlet_psi) && inlet_psi < inlet_pressure_demand_floor)
    votes++;

  if (!std::isnan(pump_flow) && pump_flow < pump_flow_collapse_threshold)
    votes++;

  if (!suppress_transient_votes) {
    if (!std::isnan(current_deriv) &&
        std::fabs(current_deriv) > motor_current_spike_threshold)
      votes++;

    if (!std::isnan(power_deriv) &&
        power_deriv > pump_power_spike_threshold)
      votes++;

    if (votes >= 1 && head_rate_peak > pump_head_rate_threshold)
      votes++;
  }

  if (votes == 0)
    return 0.0f;

  static const float conf_map[7] = {0.0f, 0.50f, 0.65f, 0.80f,
                                    0.90f, 0.95f, 0.95f};
  return (votes < 7) ? conf_map[votes] : 0.95f;
}

void test_startup_transients_are_suppressed() {
  std::cout << "\n=== Testing Startup Transient Suppression ===" << std::endl;

  // Mirrors the false-positive trace from Home Assistant:
  // inlet pressure jumps from 0 -> ~13 PSI and current from 0 -> ~0.155 A
  // when the recirculation pump starts, but there is no low-pressure or
  // flow-collapse evidence of an open-loop DHW draw.
  float confidence =
      deterministic_pump_on_conf(1.32f, 13.2f, 9.56f, 0.015f, 1.4f, 2.94f, true);

  TEST_ASSERT_NEAR(confidence, 0.0f, 0.0001f,
                   "Startup-only transients do not declare demand");
}

void test_startup_transients_still_trigger_without_guard() {
  std::cout << "\n=== Testing Unsuppressed Startup Transients ===" << std::endl;

  float confidence = deterministic_pump_on_conf(1.32f, 13.2f, 9.56f, 0.015f,
                                                1.4f, 2.94f, false);

  TEST_ASSERT_NEAR(confidence, 0.65f, 0.0001f,
                   "Pressure + current startup spikes would have caused a false positive");
}

void test_startup_guard_keeps_open_loop_signals() {
  std::cout << "\n=== Testing Open-Loop Signals During Startup Guard ==="
            << std::endl;

  float low_pressure_conf = deterministic_pump_on_conf(
      1.32f, 2.5f, 9.56f, 0.015f, 1.4f, 2.94f, true);
  float flow_collapse_conf = deterministic_pump_on_conf(
      1.32f, 13.2f, 0.05f, 0.015f, 1.4f, 2.94f, true);

  TEST_ASSERT_NEAR(low_pressure_conf, 0.50f, 0.0001f,
                   "Low inlet pressure still counts during startup suppression");
  TEST_ASSERT_NEAR(flow_collapse_conf, 0.50f, 0.0001f,
                   "Pump-flow collapse still counts during startup suppression");
}

int main() {
  std::cout << "==========================================================="
            << std::endl;
  std::cout << "  DHW Demand Pump-On Logic Test Suite" << std::endl;
  std::cout << "==========================================================="
            << std::endl;

  test_startup_transients_are_suppressed();
  test_startup_transients_still_trigger_without_guard();
  test_startup_guard_keeps_open_loop_signals();

  std::cout << "\n==========================================================="
            << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "==========================================================="
            << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  if (tests_failed == 0) {
    std::cout << "\n✓ ALL TESTS PASSED!" << std::endl;
    return 0;
  }

  std::cout << "\n✗ SOME TESTS FAILED!" << std::endl;
  return 1;
}
