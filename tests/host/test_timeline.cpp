// Host tests for anchor resolution and bucket realignment.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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

static void test_local_days_across_daylight_saving() {
  // Sydney: AEST (UTC+10), AEDT (UTC+11) from the first Sunday in October
  // 02:00 to the first Sunday in April 03:00.
  setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
  tzset();

  // Sunday 4 October 2026: clocks go forward, a 23-hour day.
  const uint32_t oct4_midnight = 1791036000;  // 2026-10-03 14:00 UTC
  const uint32_t oct4_noon = 1791075600;      // 12:00 AEDT, 11 h after midnight
  LocalTime lt = local_time(oct4_noon);
  EXPECT(lt.date == 20261004 && lt.minute == 12 * 60);
  EXPECT(lt.day_start == oct4_midnight);
  EXPECT(next_local_midnight(oct4_midnight) - oct4_midnight == 23 * 3600);

  // Sunday 5 April 2026: clocks go back, a 25-hour day.
  const uint32_t apr5_midnight = 1775307600;  // 2026-04-04 13:00 UTC
  const uint32_t apr5_noon = 1775354400;      // 12:00 AEST, 13 h after midnight
  lt = local_time(apr5_noon);
  EXPECT(lt.date == 20260405 && lt.minute == 12 * 60);
  EXPECT(lt.day_start == apr5_midnight);
  EXPECT(next_local_midnight(apr5_midnight) - apr5_midnight == 25 * 3600);

  // An ordinary day.
  EXPECT(next_local_midnight(local_time(oct4_noon + 86400).day_start) - local_time(oct4_noon + 86400).day_start ==
         24 * 3600);

  setenv("TZ", "UTC0", 1);
  tzset();
}

// ESPHome on the device: TZ stays unset (libc sees UTC) and a converter
// supplies local time. Here a fixed UTC+11 stands in for it.
static bool fixed_plus_11(time_t t, struct tm *out) {
  const time_t local = t + 11 * 3600;
  return gmtime_r(&local, out) != nullptr;
}

static void test_converter_ignores_tz() {
  setenv("TZ", "UTC0", 1);
  tzset();
  set_local_converter(fixed_plus_11);
  // 2026-11-23 00:00 UTC is 11:00 local on the 23rd; local midnight was
  // 2026-11-22 13:00 UTC.
  const uint32_t utc_midnight = 1795392000;
  LocalTime lt = local_time(utc_midnight);
  EXPECT(lt.date == 20261123 && lt.minute == 11 * 60);
  EXPECT(lt.day_start == utc_midnight - 11 * 3600);
  EXPECT(next_local_midnight(lt.day_start) == lt.day_start + 86400);
  // 10:59 local on the 23rd is still the 23rd, not the 22nd as UTC would say.
  lt = local_time(utc_midnight - 60);
  EXPECT(lt.date == 20261123 && lt.day_start == utc_midnight - 11 * 3600);
  set_local_converter(nullptr);
}

int main() {
  test_local_days_across_daylight_saving();
  test_converter_ignores_tz();
  test_anchor_resolution();
  test_aligned_interval_is_one_bucket();
  test_untimed_interval_splits_by_time();
  test_long_interval_spans_buckets();
  test_zero_duration();
  if (failures == 0)
    std::printf("All timeline tests passed\n");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
