#include "web_handler.h"

#include <cstdlib>
#include <cstring>

namespace esphome::wattledger {

static const char *const PREFIX = "/wattledger/v1/";

void WebHandler::setup() {
  this->base_->init();
  this->base_->add_handler(this);
}

bool WebHandler::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET)
    return false;
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string url = request->url_to(url_buf).str();
  return url.rfind(PREFIX, 0) == 0;
}

static uint32_t param_u32(AsyncWebServerRequest *request, const char *name, uint32_t fallback) {
  AsyncWebParameter *param = request->getParam(name);
  return param == nullptr ? fallback : strtoul(param->value().c_str(), nullptr, 10);
}

void WebHandler::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string path = request->url_to(url_buf).str().substr(strlen(PREFIX));

  if (path == "info") {
    const std::string body = this->ledger_->info_json();
    request->send(200, "application/json", body.c_str());
    return;
  }
  if (path == "rows") {
    AsyncWebParameter *tier = request->getParam("tier");
    std::string body;
    if (tier == nullptr ||
        !this->ledger_->rows_json(tier->value(), param_u32(request, "from_seq", 0), param_u32(request, "limit", 0),
                                  body)) {
      request->send(400, "text/plain", "tier must be min1, min5 or events, and the journal must be available");
      return;
    }
    request->send(200, "application/json", body.c_str());
    return;
  }
  request->send(404, "text/plain", "Not found");
}

}  // namespace esphome::wattledger
