#include "web_token.h"

#include <cJSON.h>
#include <esp_random.h>

#include <mutex>

#include "common.h"
#include "storage.h"
#include "web_token_core.h"

namespace vat::web_token {
namespace {

constexpr char kSecrets[] = "/vat/secrets.json";
std::mutex lock;
String cached;

bool save(const String& value) {
    cJSON* secrets = cJSON_Parse(storage::read_text(kSecrets, 32768).c_str());
    if (!secrets) secrets = cJSON_CreateObject();
    cJSON* web = json_obj(secrets, "web");
    if (!web) web = cJSON_AddObjectToObject(secrets, "web");
    cJSON_DeleteItemFromObject(web, "token");
    cJSON_AddStringToObject(web, "token", value.c_str());
    const bool ok = storage::write_text(kSecrets, json_print(secrets));
    cJSON_Delete(secrets);
    return ok;
}

String fresh() {
    uint8_t bytes[kBytes];
    esp_fill_random(bytes, sizeof bytes);
    return encode(bytes, sizeof bytes);
}

}  // namespace

String token() {
    std::lock_guard<std::mutex> guard(lock);
    if (!cached.isEmpty()) return cached;
    cJSON* secrets = cJSON_Parse(storage::read_text(kSecrets, 32768).c_str());
    cached = json_str(json_obj(secrets, "web"), "token");
    cJSON_Delete(secrets);
    if (cached.length() < 20) {
        cached = fresh();
        if (!save(cached)) runtime_event("web_token", "could not save a new token");
    }
    return cached;
}

String regenerate() {
    const String value = fresh();
    std::lock_guard<std::mutex> guard(lock);
    if (save(value)) cached = value;
    return cached;
}

bool accepts(const String& presented) { return same(presented, token()); }

}  // namespace vat::web_token
