#pragma once

#include "esphome/core/component.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include "wattledger.h"

namespace esphome::wattledger {

/// History API under /wattledger/v1/ (design section 9.1):
///   GET info                               layout, tiers and their sequence ranges
///   GET rows?tier=min1&from_seq=N&limit=M  records of a tier (min1, min5, events)
class WebHandler : public AsyncWebHandler, public Component {
 public:
  WebHandler(web_server_base::WebServerBase *base, WattLedgerComponent *ledger) : base_(base), ledger_(ledger) {}

  void setup() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  web_server_base::WebServerBase *base_;
  WattLedgerComponent *ledger_;
};

}  // namespace esphome::wattledger
