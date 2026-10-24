// Host tests for building and formatting PVOutput statuses.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "pvoutput_status.h"

using namespace esphome::wattledger;

static int failures = 0;

#define EXPECT(cond)                                              \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                 \
    }                                                             \
  } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// UTC as local time. 2026-10-24 00:00:00 UTC = 1792800000.
static const uint32_t DAY = 1792800000;
static LocalTime utc(uint32_t t) {
  const uint32_t day_start = t - t % 86400;
  return {20261024 + (day_start - DAY) / 86400, static_cast<uint16_t>((t - day_start) / 60), day_start};
}

static PlacedRecord rec(uint32_t seq, uint32_t start, double gen, double cons = 0, uint32_t dur = 300,
                        float v = NAN) {
  return {seq, start, dur, gen, cons, v};
}

static void test_aligned_records_one_status_each() {
  DayTotals day;
  uint32_t sent = 0;
  size_t consumed;
  auto out = build_statuses({rec(1, DAY + 600, 10, 20), rec(2, DAY + 900, 30, 40)}, 30, utc, day, sent, consumed);
  EXPECT(out.size() == 2 && consumed == 2);
  EXPECT(near(out[1].day_generation_wh, 40) && near(out[1].day_consumption_wh, 60));
  EXPECT(format_status(out[0], utc, true) == "20261024,00:15,10,120,20,240");
}

static void test_prior_day_totals_and_new_day() {
  DayTotals day{DAY, 1000, 0};
  uint32_t sent = 0;
  size_t consumed;
  auto out = build_statuses({rec(1, DAY + 86400 - 300, 5), rec(2, DAY + 86400, 7)}, 30, utc, day, sent, consumed);
  EXPECT(out.size() == 2);
  EXPECT(near(out[0].day_generation_wh, 1005));
  EXPECT(near(out[1].day_generation_wh, 7));  // a new day starts at zero
  // The last status of a day is reported at 23:59 of that day.
  EXPECT(format_status(out[0], utc, false) == "20261024,23:59,1005,60,-1,-1");
}

static void test_unaligned_records_split_across_batches() {
  DayTotals day;
  uint32_t sent = 0;
  size_t consumed;
  // 300 s from 00:02:30 covers half of two buckets; only the first is complete.
  auto out = build_statuses({rec(1, DAY + 150, 10)}, 30, utc, day, sent, consumed);
  EXPECT(out.size() == 1 && near(out[0].generation_wh, 5));
  EXPECT(sent == DAY + 300 && consumed == 0);  // record 1 isn't fully sent yet
  // Next batch, still from record 1: its first half isn't sent again.
  out = build_statuses({rec(1, DAY + 150, 10), rec(2, DAY + 450, 10)}, 30, utc, day, sent, consumed);
  EXPECT(out.size() == 1 && near(out[0].generation_wh, 10) && out[0].start_unix == DAY + 300);
  EXPECT(near(out[0].day_generation_wh, 15));
  EXPECT(consumed == 1);  // record 2 continues into an incomplete bucket
}

static void test_batch_limit() {
  DayTotals day;
  uint32_t sent = 0;
  size_t consumed;
  auto out = build_statuses({rec(1, DAY, 1), rec(2, DAY + 300, 2, 0, 600), rec(3, DAY + 900, 3)}, 2, utc, day,
                            sent, consumed);
  EXPECT(out.size() == 2 && sent == DAY + 600 && consumed == 1);
  out = build_statuses({rec(2, DAY + 300, 2, 0, 600), rec(3, DAY + 900, 3)}, 2, utc, day, sent, consumed);
  EXPECT(out.size() == 2 && out[0].start_unix == DAY + 600 && consumed == 2);
  EXPECT(near(out[1].day_generation_wh, 6));
}

static void test_voltage() {
  DayTotals day;
  uint32_t sent = 0;
  size_t consumed;
  auto out = build_statuses({rec(1, DAY, 1, 0, 300, 240.0f)}, 30, utc, day, sent, consumed);
  EXPECT(format_status(out[0], utc, true) == "20261024,00:05,1,12,0,0,-1,240.0");
}

static void test_response_parsing() {
  EXPECT(count_status_results("20261024,10:00,1;20261024,10:05,0") == 2);
  EXPECT(count_status_results("20261024,10:00,1\n") == 1);
  EXPECT(count_status_results("Bad request 400: Date is older than 14 days [20261001]") == -1);
}

int main() {
  test_aligned_records_one_status_each();
  test_prior_day_totals_and_new_day();
  test_unaligned_records_split_across_batches();
  test_batch_limit();
  test_voltage();
  test_response_parsing();
  if (failures == 0)
    std::printf("All PVOutput tests passed\n");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
