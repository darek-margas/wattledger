// Host tests for sealing days from 5-minute records.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "journal.h"
#include "records.h"
#include "sealing.h"

using namespace esphome::wattledger;

static int failures = 0;

#define EXPECT(cond)                                              \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                 \
    }                                                             \
  } while (0)

static const uint32_t DAY = 1792800000;  // 2026-10-24 00:00 UTC

static IntervalRecord rec(uint32_t start, uint32_t mwh, int32_t cost, uint8_t flags = 0, uint32_t defs = 7,
                          uint16_t coverage = 300) {
  IntervalRecord r;
  r.flags = flags;
  r.defs_hash = defs;
  r.start = start;
  r.duration_s = 300;
  r.coverage_s = coverage;
  r.input_import_mwh = {mwh};
  r.input_export_mwh = {0};
  r.output_import_mwh = {mwh, 0};
  r.output_export_mwh = {0, 0};
  r.line_dv = {2400};
  r.cost = cost;
  return r;
}

static void test_full_day() {
  DaySealer sealer(1, 2, 1, 20261024, DAY, DAY + 86400, 7);
  for (uint32_t i = 0; i < 288; i++)
    sealer.add(rec(DAY + 300 * i, 1000, 300), DAY + 300 * i);
  const DayRecord d = sealer.finish(1.10);
  EXPECT(d.records == 288 && d.coverage_s == 86400 && d.length_s == 86400);
  EXPECT(d.flags == 0);
  EXPECT(d.input_import_mwh[0] == 288000 && d.output_import_mwh[0] == 288000);
  EXPECT(d.line_dv[0] == 2400);
  EXPECT(d.cost == 288 * 300 && d.supply_charge == 1100000);
}

static void test_partial_day_and_midnight_crossing() {
  DaySealer sealer(1, 2, 1, 20261024, DAY, DAY + 86400, 7);
  // An untimed record placed at 23:57:30 crosses midnight: half of it counts.
  sealer.add(rec(0, 1000, 100, FLAG_UNTIMED | FLAG_PRICE_SCHEDULE), DAY + 86400 - 150);
  // A record outside the day doesn't count at all.
  sealer.add(rec(0, 1000, 100), DAY + 86400);
  // One made under other output definitions.
  sealer.add(rec(0, 1000, 100, 0, 99), DAY);
  const DayRecord d = sealer.finish(0);
  EXPECT(d.records == 2);
  EXPECT(d.input_import_mwh[0] == 1500);
  EXPECT(d.cost == 150);
  EXPECT(d.price_schedule_s == 150);
  EXPECT((d.flags & DAY_PARTIAL) && (d.flags & DAY_MIXED_DEFS));
}

static void test_day_record_round_trip() {
  DaySealer sealer(1, 2, 1, 20261024, DAY, DAY + 86400, 7);
  sealer.add(rec(DAY, 1234, -5), DAY);
  DayRecord in = sealer.finish(0.5);
  in.credit = 42;
  DayLayout layout(1, 2, 1);
  std::vector<uint8_t> buf(layout.size());
  layout.encode(in, buf.data());
  DayRecord out;
  layout.decode(buf.data(), out);
  EXPECT(out.date == 20261024 && out.day_start == DAY && out.length_s == 86400);
  EXPECT(out.input_import_mwh[0] == 1234 && out.output_import_mwh[0] == 1234 && out.output_import_mwh[1] == 0);
  EXPECT(out.cost == -5 && out.credit == 42 && out.supply_charge == 500000);
  EXPECT(out.records == 1 && out.flags == in.flags && out.defs_hash == 7);
}

int main() {
  test_full_day();
  test_partial_day_and_midnight_crossing();
  test_day_record_round_trip();
  if (failures == 0)
    std::printf("All sealing tests passed\n");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
