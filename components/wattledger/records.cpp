#include "records.h"

#include <cstring>

#include "journal.h"

namespace esphome::wattledger {

namespace {

class Writer {
 public:
  explicit Writer(uint8_t *p) : p_(p) {}
  void u8(uint8_t v) { *p_++ = v; }
  void u16(uint16_t v) {
    u8(v & 0xFF);
    u8(v >> 8);
  }
  void u32(uint32_t v) {
    u16(v & 0xFFFF);
    u16(v >> 16);
  }
  void f32(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    u32(bits);
  }

 protected:
  uint8_t *p_;
};

class Reader {
 public:
  explicit Reader(const uint8_t *p) : p_(p) {}
  uint8_t u8() { return *p_++; }
  uint16_t u16() {
    const uint16_t lo = u8();
    return lo | (uint16_t(u8()) << 8);
  }
  uint32_t u32() {
    const uint32_t lo = u16();
    return lo | (uint32_t(u16()) << 16);
  }
  float f32() {
    const uint32_t bits = u32();
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }

 protected:
  const uint8_t *p_;
};

}  // namespace

// Header: magic u16, version u8, flags u8, seq u32 (ring header), then
// boot_id u16, defs_hash u32, start u32, duration_s u16, coverage_s u16.
static const size_t INTERVAL_HEADER_SIZE = RECORD_HEADER_SIZE + 14;

size_t IntervalLayout::size() const {
  return INTERVAL_HEADER_SIZE + 8 * this->inputs_ + 8 * this->output_slots_ + 2 * this->lines_ + 16 +
         RECORD_CRC_SIZE;
}

void IntervalLayout::encode(const IntervalRecord &rec, uint8_t *out) const {
  std::memset(out, 0, this->size());
  Writer w(out + 2);
  w.u8(RECORD_VERSION);
  w.u8(rec.flags);
  w.u32(0);  // sequence number, set by the ring
  w.u16(rec.boot_id);
  w.u32(rec.defs_hash);
  w.u32(rec.start);
  w.u16(rec.duration_s);
  w.u16(rec.coverage_s);
  for (size_t i = 0; i < this->inputs_; i++) {
    w.u32(i < rec.input_import_mwh.size() ? rec.input_import_mwh[i] : 0);
    w.u32(i < rec.input_export_mwh.size() ? rec.input_export_mwh[i] : 0);
  }
  for (size_t o = 0; o < this->output_slots_; o++) {
    w.u32(o < rec.output_import_mwh.size() ? rec.output_import_mwh[o] : 0);
    w.u32(o < rec.output_export_mwh.size() ? rec.output_export_mwh[o] : 0);
  }
  for (size_t l = 0; l < this->lines_; l++)
    w.u16(l < rec.line_dv.size() ? rec.line_dv[l] : 0);
  w.f32(rec.price_import);
  w.f32(rec.price_export);
  w.u32(static_cast<uint32_t>(rec.cost));
  w.u32(static_cast<uint32_t>(rec.credit));
}

void IntervalLayout::decode(const uint8_t *in, IntervalRecord &rec) const {
  Reader r(in + 2);
  r.u8();  // version
  rec.flags = r.u8();
  rec.seq = r.u32();
  rec.boot_id = r.u16();
  rec.defs_hash = r.u32();
  rec.start = r.u32();
  rec.duration_s = r.u16();
  rec.coverage_s = r.u16();
  rec.input_import_mwh.resize(this->inputs_);
  rec.input_export_mwh.resize(this->inputs_);
  for (size_t i = 0; i < this->inputs_; i++) {
    rec.input_import_mwh[i] = r.u32();
    rec.input_export_mwh[i] = r.u32();
  }
  rec.output_import_mwh.resize(this->output_slots_);
  rec.output_export_mwh.resize(this->output_slots_);
  for (size_t o = 0; o < this->output_slots_; o++) {
    rec.output_import_mwh[o] = r.u32();
    rec.output_export_mwh[o] = r.u32();
  }
  rec.line_dv.resize(this->lines_);
  for (size_t l = 0; l < this->lines_; l++)
    rec.line_dv[l] = r.u16();
  rec.price_import = r.f32();
  rec.price_export = r.f32();
  rec.cost = static_cast<int32_t>(r.u32());
  rec.credit = static_cast<int32_t>(r.u32());
}

// Event: ring header with type in the flags byte, then boot_id u16, pad u16,
// mono_s u32, unix_s u32, a u32, b u32, CRC.
void encode_event(const EventRecord &ev, uint8_t *out) {
  std::memset(out, 0, EVENT_RECORD_SIZE);
  Writer w(out + 2);
  w.u8(RECORD_VERSION);
  w.u8(ev.type);
  w.u32(0);
  w.u16(ev.boot_id);
  w.u16(0);
  w.u32(ev.mono_s);
  w.u32(ev.unix_s);
  w.u32(ev.a);
  w.u32(ev.b);
}

void decode_event(const uint8_t *in, EventRecord &ev) {
  Reader r(in + 2);
  r.u8();
  ev.type = r.u8();
  ev.seq = r.u32();
  ev.boot_id = r.u16();
  r.u16();
  ev.mono_s = r.u32();
  ev.unix_s = r.u32();
  ev.a = r.u32();
  ev.b = r.u32();
}

}  // namespace esphome::wattledger
