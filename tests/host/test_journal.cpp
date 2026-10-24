// Host tests for the journal rings and record layouts.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "journal.h"
#include "records.h"

using namespace esphome::wattledger;

static int failures = 0;

#define EXPECT(cond)                                         \
  do {                                                       \
    if (!(cond)) {                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                            \
    }                                                        \
  } while (0)

/// NOR flash in RAM: writes only clear bits. `torn_after` cuts the next
/// write short after that many bytes, like a power loss.
class MemStorage : public Storage {
 public:
  explicit MemStorage(uint32_t sectors) : data_(sectors * SECTOR_SIZE, 0xFF) {}
  uint32_t size() const override { return this->data_.size(); }
  bool read(uint32_t offset, void *data, size_t len) override {
    std::memcpy(data, &this->data_[offset], len);
    return true;
  }
  bool write(uint32_t offset, const void *data, size_t len) override {
    if (this->torn_after >= 0 && static_cast<size_t>(this->torn_after) < len)
      len = this->torn_after;
    this->torn_after = -1;
    const uint8_t *src = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; i++)
      this->data_[offset + i] &= src[i];
    return true;
  }
  bool erase_sector(uint32_t offset) override {
    std::memset(&this->data_[offset], 0xFF, SECTOR_SIZE);
    this->erases++;
    return true;
  }
  int torn_after{-1};
  int erases{0};

 protected:
  std::vector<uint8_t> data_;
};

static void fill(std::vector<uint8_t> &rec, uint8_t value) {
  for (size_t i = 8; i < rec.size() - 4; i++)
    rec[i] = value;
}

static void test_crc32() {
  const char *s = "123456789";
  EXPECT(crc32(0, reinterpret_cast<const uint8_t *>(s), 9) == 0xCBF43926);
}

static void test_append_read_wrap() {
  MemStorage st(3);
  Ring ring;
  ring.configure(&st, 0, 3, 1024, 7);  // 4 records per sector, capacity 12
  std::vector<uint8_t> rec(1024), out(1024);
  for (uint32_t i = 1; i <= 30; i++) {
    fill(rec, static_cast<uint8_t>(i));
    EXPECT(ring.append(rec.data()) == i);
  }
  EXPECT(ring.next_seq() == 31);
  // Seq 29-30 are in the current sector; the other two sectors hold 21-28.
  EXPECT(ring.first_seq() == 21);
  EXPECT(!ring.read(20, out.data()));
  EXPECT(ring.read(21, out.data()) && out[100] == 21);
  EXPECT(ring.read(30, out.data()) && out[100] == 30);
  EXPECT(!ring.read(31, out.data()));
}

static void test_scan_after_reboot() {
  MemStorage st(3);
  Ring ring;
  ring.configure(&st, 0, 3, 1024, 7);
  std::vector<uint8_t> rec(1024);
  for (uint32_t i = 1; i <= 18; i++)
    ring.append(rec.data());
  Ring again;
  again.configure(&st, 0, 3, 1024, 7);
  EXPECT(again.scan());
  EXPECT(again.next_seq() == 19);
  EXPECT(again.first_seq() == ring.first_seq());
}

static void test_torn_write_is_skipped() {
  MemStorage st(3);
  Ring ring;
  ring.configure(&st, 0, 3, 1024, 7);
  std::vector<uint8_t> rec(1024), out(1024);
  for (uint32_t i = 1; i <= 5; i++)
    ring.append(rec.data());
  st.torn_after = 300;
  ring.append(rec.data());  // seq 6, torn

  Ring again;
  again.configure(&st, 0, 3, 1024, 7);
  EXPECT(again.scan());
  EXPECT(again.next_seq() == 6);
  EXPECT(!again.read(6, out.data()));
  // The torn slot can't be rewritten without an erase: it's skipped.
  EXPECT(again.append(rec.data()) == 7);
  EXPECT(again.read(7, out.data()));
  EXPECT(again.read(5, out.data()));
}

static void test_other_salt_is_invalid() {
  MemStorage st(3);
  Ring ring;
  ring.configure(&st, 0, 3, 1024, 7);
  std::vector<uint8_t> rec(1024);
  for (int i = 0; i < 5; i++)
    ring.append(rec.data());
  Ring other;
  other.configure(&st, 0, 3, 1024, 8);
  EXPECT(other.scan());
  EXPECT(other.next_seq() == 1);
}

static void test_journal_layout_change() {
  MemStorage st(64);
  Journal j;
  EXPECT(j.begin(&st, 200, 100, 200, 64));
  EXPECT(j.started_new());
  std::vector<uint8_t> rec(200);
  j.min1().append(rec.data());
  j.min1().append(rec.data());

  Journal same;
  EXPECT(same.begin(&st, 200, 100, 200, 64));
  EXPECT(!same.started_new());
  EXPECT(same.min1().next_seq() == 3);

  Journal changed;
  EXPECT(changed.begin(&st, 208, 100, 200, 64));
  EXPECT(changed.started_new());
  EXPECT(changed.min1().next_seq() == 1);

  MemStorage small(4);
  Journal too_big;
  EXPECT(!too_big.begin(&small, 200, 100, 200, 64));
}

static void test_interval_record_round_trip() {
  IntervalLayout layout(2, 3, 1);
  IntervalRecord in;
  in.flags = FLAG_PARTIAL | FLAG_UNTIMED;
  in.boot_id = 9;
  in.defs_hash = 0xDEADBEEF;
  in.start = 1700000000;
  in.duration_s = 60;
  in.coverage_s = 58;
  in.input_import_mwh = {1, 2};
  in.input_export_mwh = {3, 4};
  in.output_import_mwh = {5, 6};  // third slot unused
  in.output_export_mwh = {7, 8};
  in.line_dv = {2405};
  in.price_import = 0.25f;
  in.cost = -42;
  std::vector<uint8_t> buf(layout.size());
  layout.encode(in, buf.data());

  MemStorage st(2);
  Ring ring;
  ring.configure(&st, 0, 2, layout.size(), 1);
  const uint32_t seq = ring.append(buf.data());
  std::vector<uint8_t> raw(layout.size());
  EXPECT(ring.read(seq, raw.data()));
  IntervalRecord out;
  layout.decode(raw.data(), out);
  EXPECT(out.seq == seq);
  EXPECT(out.flags == in.flags);
  EXPECT(out.boot_id == 9 && out.defs_hash == 0xDEADBEEF && out.start == 1700000000);
  EXPECT(out.duration_s == 60 && out.coverage_s == 58);
  EXPECT(out.input_export_mwh[1] == 4);
  EXPECT(out.output_import_mwh[1] == 6 && out.output_import_mwh[2] == 0);
  EXPECT(out.line_dv[0] == 2405);
  EXPECT(out.price_import == 0.25f && out.cost == -42);
}

static void test_event_round_trip() {
  EventRecord in;
  in.type = EVENT_ANCHOR;
  in.boot_id = 3;
  in.mono_s = 120;
  in.unix_s = 1700000123;
  in.a = 1;
  in.b = 2;
  uint8_t buf[EVENT_RECORD_SIZE];
  encode_event(in, buf);
  EventRecord out;
  decode_event(buf, out);
  EXPECT(out.type == EVENT_ANCHOR && out.boot_id == 3 && out.mono_s == 120 && out.unix_s == 1700000123);
  EXPECT(out.a == 1 && out.b == 2);
}

int main() {
  test_crc32();
  test_append_read_wrap();
  test_scan_after_reboot();
  test_torn_write_is_skipped();
  test_other_salt_is_invalid();
  test_journal_layout_change();
  test_interval_record_round_trip();
  test_event_round_trip();
  if (failures == 0)
    std::printf("All journal tests passed\n");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
