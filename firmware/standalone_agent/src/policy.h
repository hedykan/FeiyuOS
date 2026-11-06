#pragma once

#include <Arduino.h>
#include <cJSON.h>

namespace vat::policy {

enum class Decision { Allow, Ask, Deny };

struct Result {
    Decision decision;
    String reason;
};

// Session-scoped allow/ask/deny; hard denials (credential paths) survive
// allow_all and approval.
Result evaluate(const String& tool, const cJSON* arguments);
bool hard_denied(const cJSON* arguments);
void set_mode(const String& target, const String& mode);
String mode_for(const String& target);

}  // namespace vat::policy
