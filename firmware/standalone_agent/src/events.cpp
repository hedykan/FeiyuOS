#include "events.h"

#include <vector>

#include "common.h"

namespace vat::events {
namespace {

std::vector<Handler> handlers;  // filled during setup, read-only afterwards

}  // namespace

void subscribe(Handler handler) {
    if (handler) handlers.push_back(handler);
}

void emit(const String& target, const String& data) {
    runtime_event("event", target);
    for (const Handler handler : handlers) handler(target, data);
}

}  // namespace vat::events
