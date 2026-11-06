// Chip-specific half of common.h (reset reason, runtime log, cancellation);
// the portable helpers are in common_text.cpp.
#include "common.h"

#include "storage.h"

#include <esp_system.h>

namespace vat {

String reset_reason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB: return "usb";
        default: return "other";
    }
}

void runtime_event(const char* event, const String& detail) {
    cJSON* record = cJSON_CreateObject();
    cJSON_AddNumberToObject(record, "millis", millis());
    cJSON_AddStringToObject(record, "event", event);
    cJSON_AddNumberToObject(record, "heap", ESP.getFreeHeap());
    cJSON_AddNumberToObject(record, "max_block", ESP.getMaxAllocHeap());
    cJSON_AddNumberToObject(record, "min_heap", ESP.getMinFreeHeap());  // low-water mark since boot
    if (!detail.isEmpty()) cJSON_AddStringToObject(record, "detail", utf8_head(detail, 600).c_str());
    const String line = json_print(record);
    Serial.println(line);  // USB console mirror for development
    storage::append_buffered("/vat/logs/runtime.jsonl", line + "\n", true);
    cJSON_Delete(record);
}

}  // namespace vat

#include <atomic>
#include <lwip/sockets.h>

namespace vat::cancel {
namespace {
std::atomic<bool> flag{false};
std::atomic<int> active_fd{-1};
}  // namespace

void reset() { flag = false; }

void request() {
    flag = true;
    const int fd = active_fd.load();
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}

bool requested() { return flag.load(); }
void set_socket(int fd) { active_fd = fd; }
void clear_socket() { active_fd = -1; }

}  // namespace vat::cancel
