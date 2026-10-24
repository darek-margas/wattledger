// Host tests for the accumulator: g++ -std=c++17 -I components/wattledger ...
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "accumulator.h"

using namespace esphome::wattledger;

static int failures = 0;

static void expect_near(double actual, double expected, const char *what) {
  if (std::fabs(actual - expected) > 1e-9) {
    std::printf("FAIL %s: got %.12f, expected %.12f\n", what, actual, expected);
    failures++;
  }
}

static float grid(const float *in) { return in[0]; }
static float export_only(const float *in) { return in[0] < 0 ? -in[0] : 0.0f; }

static void test_split_and_per_sample_clamp() {
  Accumulator acc;
  acc.configure(1, 0, {grid, export_only});
  // One hour alternating import and export of 1 kW, one second at a time.
  for (int s = 0; s < 3600; s++) {
    const float w = (s % 2 == 0) ? 1000.0f : -1000.0f;
    acc.integrate(&w, nullptr, 1000, true);
  }
  const Totals &t = acc.totals();
  expect_near(t.input_import_wh[0], 500.0, "input import");
  expect_near(t.input_export_wh[0], 500.0, "input export");
  expect_near(t.output_import_wh[0], 500.0, "grid import");
  expect_near(t.output_export_wh[0], 500.0, "grid export");
  // Clamped per sample: the export is all there, though the net is zero.
  expect_near(t.output_import_wh[1], 500.0, "export-only output");
  expect_near(t.output_net_wh(0), 0.0, "grid net");
}

static void test_invalid_time_is_a_gap() {
  Accumulator acc;
  acc.configure(1, 0, {grid});
  const float w = 3600.0f;
  acc.integrate(&w, nullptr, 1000, true);
  acc.integrate(&w, nullptr, 5000, false);
  const Totals &t = acc.totals();
  expect_near(t.output_import_wh[0], 1.0, "only valid time integrated");
  expect_near(t.valid_ms, 1000, "valid ms");
  expect_near(t.total_ms, 6000, "total ms");
}

static void test_lines_and_delta() {
  Accumulator acc;
  acc.configure(1, 1, {grid});
  const float w = 0.0f;
  float v = 240.0f;
  acc.integrate(&w, &v, 2000, true);
  const Totals first = acc.totals();
  v = NAN;
  acc.integrate(&w, &v, 1000, true);
  v = 230.0f;
  acc.integrate(&w, &v, 1000, false);
  const Totals d = delta(acc.totals(), first);
  expect_near(d.line_volt_seconds[0], 230.0, "volt-seconds after first");
  expect_near(d.line_valid_ms[0], 1000, "line valid ms after first");
  expect_near(d.total_ms, 2000, "delta total ms");
  expect_near(d.valid_ms, 1000, "delta valid ms");
}

int main() {
  test_split_and_per_sample_clamp();
  test_invalid_time_is_a_gap();
  test_lines_and_delta();
  if (failures == 0)
    std::printf("All accumulator tests passed\n");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
