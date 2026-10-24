#pragma once

#ifdef USE_ESP32

#include <esp_partition.h>

#include "journal.h"

namespace esphome::wattledger {

/// The journal's flash partition.
class PartitionStorage : public Storage {
 public:
  bool open(const char *label) {
    this->partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    return this->partition_ != nullptr;
  }
  uint32_t size() const override { return this->partition_->size; }
  bool read(uint32_t offset, void *data, size_t len) override {
    return esp_partition_read(this->partition_, offset, data, len) == ESP_OK;
  }
  bool write(uint32_t offset, const void *data, size_t len) override {
    return esp_partition_write(this->partition_, offset, data, len) == ESP_OK;
  }
  bool erase_sector(uint32_t offset) override {
    return esp_partition_erase_range(this->partition_, offset, SECTOR_SIZE) == ESP_OK;
  }

 protected:
  const esp_partition_t *partition_{nullptr};
};

}  // namespace esphome::wattledger

#endif  // USE_ESP32
