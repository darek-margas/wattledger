#include "journal.h"

#include <cstring>

namespace esphome::wattledger {

uint32_t crc32(uint32_t seed, const uint8_t *data, size_t len) {
  // Reflected CRC-32 (IEEE), half-byte table.
  static const uint32_t TABLE[16] = {
      0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
      0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
  };
  uint32_t crc = ~seed;
  for (size_t i = 0; i < len; i++) {
    crc = TABLE[(crc ^ data[i]) & 0x0F] ^ (crc >> 4);
    crc = TABLE[(crc ^ (data[i] >> 4)) & 0x0F] ^ (crc >> 4);
  }
  return ~crc;
}

static uint32_t get_u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

uint32_t Ring::sectors_for(uint32_t records, uint32_t record_size) {
  const uint32_t per_sector = SECTOR_SIZE / record_size;
  // One extra sector: it is erased ahead of the write position.
  return (records + per_sector - 1) / per_sector + 1;
}

void Ring::configure(Storage *storage, uint32_t first_sector, uint32_t sectors, uint32_t record_size, uint32_t salt) {
  this->storage_ = storage;
  this->first_sector_ = first_sector;
  this->sectors_ = sectors;
  this->record_size_ = record_size;
  this->per_sector_ = SECTOR_SIZE / record_size;
  this->capacity_ = sectors * this->per_sector_;
  this->salt_ = salt;
  this->next_seq_ = 1;
  this->buffer_.assign(record_size, 0);
}

uint32_t Ring::offset_of_(uint32_t seq) const {
  const uint32_t slot = (seq - 1) % this->capacity_;
  return (this->first_sector_ + slot / this->per_sector_) * SECTOR_SIZE + (slot % this->per_sector_) * this->record_size_;
}

bool Ring::valid_(const uint8_t *record, uint32_t seq) const {
  const uint16_t magic = record[0] | (record[1] << 8);
  if (magic != RECORD_MAGIC || get_u32(record + 4) != seq)
    return false;
  const size_t body = this->record_size_ - RECORD_CRC_SIZE;
  return crc32(this->salt_, record, body) == get_u32(record + body);
}

bool Ring::read(uint32_t seq, uint8_t *record) {
  if (seq == 0 || seq >= this->next_seq_ || seq < this->first_seq())
    return false;
  if (!this->storage_->read(this->offset_of_(seq), record, this->record_size_))
    return false;
  return this->valid_(record, seq);
}

uint32_t Ring::first_seq() const {
  // Sequence number of the first slot of the sector being written.
  const uint32_t current = this->next_seq_ - 1;
  if (current == 0)
    return 1;
  const uint32_t sector_start = current - (current - 1) % this->per_sector_;
  // The other sectors hold the previous lap, minus the sector erased ahead.
  const uint32_t window = this->capacity_ - this->per_sector_;
  return sector_start > window ? sector_start - window : 1;
}

bool Ring::scan() {
  uint8_t *rec = this->buffer_.data();
  // The sector holding the newest records has the largest first sequence number.
  uint32_t best_first = 0;
  for (uint32_t s = 0; s < this->sectors_; s++) {
    if (!this->storage_->read((this->first_sector_ + s) * SECTOR_SIZE, rec, this->record_size_))
      return false;
    const uint32_t seq = get_u32(rec + 4);
    if (seq == 0 || (seq - 1) % this->capacity_ != s * this->per_sector_)
      continue;
    if (seq > best_first && this->valid_(rec, seq))
      best_first = seq;
  }
  if (best_first == 0) {
    this->next_seq_ = 1;
    return true;
  }
  uint32_t last = best_first;
  for (uint32_t seq = best_first + 1; seq < best_first + this->per_sector_; seq++) {
    if (!this->storage_->read(this->offset_of_(seq), rec, this->record_size_))
      return false;
    if (!this->valid_(rec, seq))
      break;
    last = seq;
  }
  this->next_seq_ = last + 1;
  return true;
}

bool Ring::slot_erased_(uint32_t seq) {
  uint8_t *rec = this->buffer_.data();
  if (!this->storage_->read(this->offset_of_(seq), rec, this->record_size_))
    return false;
  for (uint32_t i = 0; i < this->record_size_; i++) {
    if (rec[i] != 0xFF)
      return false;
  }
  return true;
}

uint32_t Ring::append(uint8_t *record) {
  // At most one sector's worth of slots can need skipping: those left
  // partly written by a power loss after the last valid record.
  for (uint32_t attempt = 0; attempt <= this->per_sector_; attempt++) {
    const uint32_t seq = this->next_seq_;
    const uint32_t offset = this->offset_of_(seq);
    if ((seq - 1) % this->per_sector_ == 0) {
      if (!this->storage_->erase_sector(offset))
        return 0;
    } else if (!this->slot_erased_(seq)) {
      this->next_seq_++;
      continue;
    }
    record[0] = RECORD_MAGIC & 0xFF;
    record[1] = RECORD_MAGIC >> 8;
    put_u32(record + 4, seq);
    const size_t body = this->record_size_ - RECORD_CRC_SIZE;
    put_u32(record + body, crc32(this->salt_, record, body));
    this->next_seq_++;
    if (!this->storage_->write(offset, record, this->record_size_))
      return 0;
    return seq;
  }
  return 0;
}

static const uint32_t SUPERBLOCK_MAGIC = 0x42534C57;  // "WLSB"
static const uint32_t SUPERBLOCK_VERSION = 1;

uint32_t Journal::required_size(const JournalLayout &l) {
  return (1 + Ring::sectors_for(l.event_records, 32) + Ring::sectors_for(l.min1_records, l.interval_record_size) +
          Ring::sectors_for(l.min5_records, l.interval_record_size) +
          Ring::sectors_for(l.day_records, l.day_record_size)) *
         SECTOR_SIZE;
}

bool Journal::begin(Storage *storage, const JournalLayout &l) {
  if (storage->size() < required_size(l))
    return false;
  const uint32_t ev_sectors = Ring::sectors_for(l.event_records, 32);
  const uint32_t m1_sectors = Ring::sectors_for(l.min1_records, l.interval_record_size);
  const uint32_t m5_sectors = Ring::sectors_for(l.min5_records, l.interval_record_size);
  const uint32_t day_sectors = Ring::sectors_for(l.day_records, l.day_record_size);

  uint8_t layout[28];
  put_u32(layout, SUPERBLOCK_VERSION);
  put_u32(layout + 4, l.interval_record_size);
  put_u32(layout + 8, ev_sectors);
  put_u32(layout + 12, m1_sectors);
  put_u32(layout + 16, m5_sectors);
  put_u32(layout + 20, l.day_record_size);
  put_u32(layout + 24, day_sectors);
  const uint32_t layout_hash = crc32(0, layout, sizeof(layout));

  uint8_t sb[12];
  if (!storage->read(0, sb, sizeof(sb)))
    return false;
  const bool same = get_u32(sb) == SUPERBLOCK_MAGIC && get_u32(sb + 4) == layout_hash &&
                    get_u32(sb + 8) == crc32(0, sb, 8);
  this->started_new_ = !same;
  if (!same) {
    put_u32(sb, SUPERBLOCK_MAGIC);
    put_u32(sb + 4, layout_hash);
    put_u32(sb + 8, crc32(0, sb, 8));
    if (!storage->erase_sector(0) || !storage->write(0, sb, sizeof(sb)))
      return false;
  }

  uint32_t sector = 1;
  this->events_.configure(storage, sector, ev_sectors, 32, layout_hash ^ 0x45564E54);
  sector += ev_sectors;
  this->min1_.configure(storage, sector, m1_sectors, l.interval_record_size, layout_hash ^ 0x4D494E31);
  sector += m1_sectors;
  this->min5_.configure(storage, sector, m5_sectors, l.interval_record_size, layout_hash ^ 0x4D494E35);
  sector += m5_sectors;
  this->days_.configure(storage, sector, day_sectors, l.day_record_size, layout_hash ^ 0x44415953);
  return this->events_.scan() && this->min1_.scan() && this->min5_.scan() && this->days_.scan();
}

}  // namespace esphome::wattledger
