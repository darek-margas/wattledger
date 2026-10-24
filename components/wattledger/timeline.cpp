#include "timeline.h"

namespace esphome::wattledger {

void AnchorTable::add(uint16_t boot_id, uint32_t uptime_s, uint32_t unix_s) {
  for (const Anchor &a : this->anchors_) {
    if (a.boot_id == boot_id)
      return;
  }
  this->anchors_.push_back({boot_id, uptime_s, unix_s});
}

uint32_t AnchorTable::resolve(uint16_t boot_id, uint32_t uptime_s) const {
  for (const Anchor &a : this->anchors_) {
    if (a.boot_id == boot_id)
      return a.unix_s - a.uptime_s + uptime_s;
  }
  return 0;
}

}  // namespace esphome::wattledger
