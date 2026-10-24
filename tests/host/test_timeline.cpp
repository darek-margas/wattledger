// Host tests for anchor resolution and bucket realignment.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "timeline.h"

using namespace esphome::wattledger;

static int failures = 0;

#define EXPECT(cond)                                              \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                 \
    }                                                             \
  } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-12; }

static void test_anchor_resolution() {
  AnchorTable anchors;
  anchors.add(5, 130, 1700000130);
  anchors.add(5, 900, 1800000000);  // a later anchor of the same boot is ignored
  EXPECT(anchors.size() == 1);
  EXPECT(anchors.resolve(5, 10) == 1700000010);
  EXPECT(anchors.resolve(5, 130) == 1700000130);
  EXPECT(anchors.resolve(6, 10) == 0);  // boot without a valid time
}

static void test_aligned_interval_is_one_bucket() {
  std::map<uint32_t, double> parts;
  for_each_bucket(1200, 60, 60, [&](uint32_t b, double f) { parts[b] += f; });
  EXPECT(parts.size() == 1 && near(parts[1200], 1.0));
}

static void test_untimed_interval_splits_by_time() {
  // 60 s starting 20 s into a minute: 40 s in the first bucket, 20 s in the next.
  std::map<uint32_t, double> parts;
  for_each_bucket(1220, 60, 60, [&](uint32_t b, double f) { parts[b] += f; });
  EXPECT(parts.size() == 2);
  EXPECT(near(parts[1200], 40.0 / 60));
  EXPECT(near(parts[1260], 20.0 / 60));
}

static void test_long_interval_spans_buckets() {
  // A 5-minute record realigned into 1-minute buckets.
  std::map<uint32_t, double> parts;
  double total = 0;
  for_each_bucket(1230, 300, 60, [&](uint32_t b, double f) {
    parts[b] += f;
    total += f;
  });
  EXPECT(parts.size() == 6);
  EXPECT(near(parts[1200], 30.0 / 300) && near(parts[1260], 60.0 / 300) && near(parts[1500], 30.0 / 300));
  EXPECT(near(total, 1.0));
}

static void test_zero_duration() {
  std::map<uint32_t, double> parts;
  for_each_bucket(1259, 0, 60, [&](uint32_t b, double f) { parts[b] += f; });
  EXPECT(parts.size() == 1 && near(parts[1200], 1.0));
}

int main() {
  test_anchor_resolution();
  test_aligned_interval_is_one_bucket();
  test_untimed_interval_splits_by_time();
  test_long_interval_spans_buckets();
  test_zero_duration();
  if (failures == 0)
    std::printf("All timeline tests passed\n");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
