#pragma once

// Fixed-size records in rings of flash sectors (design section 5.4).
// Independent of ESPHome so it can be unit-tested on the host.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::wattledger {

static const uint32_t SECTOR_SIZE = 4096;

/// Flash-like storage: erased bytes read 0xFF; a write can only clear bits.
class Storage {
 public:
  virtual ~Storage() = default;
  virtual uint32_t size() const = 0;
  virtual bool read(uint32_t offset, void *data, size_t len) = 0;
  virtual bool write(uint32_t offset, const void *data, size_t len) = 0;
  virtual bool erase_sector(uint32_t offset) = 0;
};

uint32_t crc32(uint32_t seed, const uint8_t *data, size_t len);

/// Every ring record starts with this header and ends with a CRC-32.
/// The CRC is seeded with the ring's salt, so records written under another
/// layout (different record size or ring placement) never read as valid.
static const uint16_t RECORD_MAGIC = 0x4C57;  // "WL"
static const size_t RECORD_HEADER_SIZE = 8;   // magic u16, version u8, flags u8, seq u32
static const size_t RECORD_CRC_SIZE = 4;

/// A ring of fixed-size records. A record's slot follows from its sequence
/// number (slot = (seq - 1) % capacity). Records never span sectors, and a
/// sector is erased just before its first slot is written, which drops the
/// oldest sector's records.
class Ring {
 public:
  void configure(Storage *storage, uint32_t first_sector, uint32_t sectors, uint32_t record_size, uint32_t salt);

  /// Finds the newest valid record, after boot. Returns false on storage errors.
  bool scan();
  /// Appends `record` (record_size bytes): sets its magic, sequence number
  /// and CRC; the caller fills version, flags and the payload. Returns the
  /// sequence number, or 0 on a storage error.
  uint32_t append(uint8_t *record);
  /// Reads the record with sequence number `seq`. False if it isn't there
  /// (overwritten, never written, or damaged by a power loss during a write).
  bool read(uint32_t seq, uint8_t *record);

  /// The oldest sequence number that may still be present.
  uint32_t first_seq() const;
  /// The sequence number the next append will get.
  uint32_t next_seq() const { return this->next_seq_; }
  uint32_t capacity() const { return this->capacity_; }
  uint32_t record_size() const { return this->record_size_; }
  uint32_t records_per_sector() const { return this->per_sector_; }

  static uint32_t sectors_for(uint32_t records, uint32_t record_size);

 protected:
  uint32_t offset_of_(uint32_t seq) const;
  bool valid_(const uint8_t *record, uint32_t seq) const;
  bool slot_erased_(uint32_t seq);

  Storage *storage_{nullptr};
  uint32_t first_sector_{0};
  uint32_t sectors_{0};
  uint32_t record_size_{0};
  uint32_t per_sector_{0};
  uint32_t capacity_{0};
  uint32_t salt_{0};
  uint32_t next_seq_{1};
  std::vector<uint8_t> buffer_;
};

/// Record sizes and counts of the journal's rings.
struct JournalLayout {
  uint32_t interval_record_size;
  uint32_t min1_records;
  uint32_t min5_records;
  uint32_t event_records;
  uint32_t day_record_size;
  uint32_t day_records;
};

/// The journal partition: a superblock sector, then the event, 1-minute,
/// 5-minute and day rings. A different layout (another record size or
/// retention) starts a new journal; old records then fail their seeded CRC.
class Journal {
 public:
  /// Bytes needed; the build sizes the partition with the same formula.
  static uint32_t required_size(const JournalLayout &layout);

  /// Opens the journal, starting a new one if the layout changed. False on
  /// storage errors or if the storage is too small.
  bool begin(Storage *storage, const JournalLayout &layout);

  Ring &events() { return this->events_; }
  Ring &min1() { return this->min1_; }
  Ring &min5() { return this->min5_; }
  Ring &days() { return this->days_; }
  /// True if begin() started a new journal.
  bool started_new() const { return this->started_new_; }

 protected:
  Ring events_;
  Ring min1_;
  Ring min5_;
  Ring days_;
  bool started_new_{false};
};

}  // namespace esphome::wattledger
