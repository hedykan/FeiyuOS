#include "ops_ir.h"

#define DISABLE_CODE_FOR_RECEIVER
#define NO_LED_FEEDBACK_CODE
#include <IRremote.hpp>
#include <cJSON.h>
#include <time.h>

#include "common.h"
#include "ops_ac.h"
#include "storage.h"

namespace vat::ir {
namespace {

enum class Proto : uint8_t { Nec, Samsung, Sony, Rc5 };

struct CodeSet {
    const char* brand;
    const char* label;
    Proto proto;
    uint16_t address;
    uint8_t power, volume_up, volume_down, mute;
};

// Widely published TV codes (address / command per protocol).
const CodeSet kSets[] = {
    {"samsung", "三星电视", Proto::Samsung, 0x07, 0x02, 0x07, 0x0B, 0x0F},
    {"lg", "LG 电视", Proto::Nec, 0x04, 0x08, 0x02, 0x03, 0x09},
    {"sony", "索尼电视", Proto::Sony, 0x01, 0x15, 0x12, 0x13, 0x14},
    {"philips", "飞利浦电视", Proto::Rc5, 0x00, 0x0C, 0x10, 0x11, 0x0D},
};
constexpr int kSetCount = sizeof(kSets) / sizeof(kSets[0]);

bool started = false;

void ensure_started() {
    if (started) return;
    IrSender.begin(kPin);
    started = true;
}

void transmit(Proto proto, uint16_t address, uint8_t command) {
    ensure_started();
    switch (proto) {
        case Proto::Nec: IrSender.sendNEC(address, command, 1); break;
        case Proto::Samsung: IrSender.sendSamsung(address, command, 1); break;
        case Proto::Sony: IrSender.sendSony(address, command, 2); break;
        case Proto::Rc5: IrSender.sendRC5(address, command, 1); break;
    }
}

const char* action_label(const String& action) {
    if (action == "power") return "电源";
    if (action == "volume_up") return "音量+";
    if (action == "volume_down") return "音量-";
    if (action == "mute") return "静音";
    return nullptr;
}

int command_for(const CodeSet& set, const String& action) {
    if (action == "power") return set.power;
    if (action == "volume_up") return set.volume_up;
    if (action == "volume_down") return set.volume_down;
    if (action == "mute") return set.mute;
    return -1;
}

int set_index(const String& brand) {
    String key = brand;
    key.toLowerCase();
    key.trim();
    for (int i = 0; i < kSetCount; ++i) {
        if (key.indexOf(kSets[i].brand) >= 0 || brand.indexOf(String(kSets[i].label).substring(0, 6)) >= 0) return i;
    }
    if (brand.indexOf("三星") >= 0) return 0;
    if (brand.indexOf("索尼") >= 0) return 2;
    if (brand.indexOf("飞利浦") >= 0) return 3;
    return -1;
}

String profile_path(const String& name) { return "/vat/things/ir/" + safe_name(name) + ".json"; }

cJSON* load(const String& name) {
    cJSON* profile = cJSON_Parse(storage::read_text(profile_path(name), 8192).c_str());
    if (!profile) {
        profile = cJSON_CreateObject();
        cJSON_AddStringToObject(profile, "name", name.c_str());
    }
    return profile;
}

void put_string(cJSON* object, const char* key, const String& value) {
    cJSON_DeleteItemFromObject(object, key);
    cJSON_AddStringToObject(object, key, value.c_str());
}

void put_number(cJSON* object, const char* key, double value) {
    cJSON_DeleteItemFromObject(object, key);
    cJSON_AddNumberToObject(object, key, value);
}

bool save(const String& name, cJSON* profile) { return storage::write_text(profile_path(name), json_print(profile)); }

// Sends with code set `index`; records it as the attempt awaiting an answer.
String attempt(const String& name, cJSON* profile, int index, const String& action) {
    const CodeSet& set = kSets[index];
    transmit(set.proto, set.address, command_for(set, action));
    put_number(profile, "trying", index);
    put_string(profile, "pending_action", action);
    put_string(profile, "last_action", action);
    put_string(profile, "last_sent", now_text());
    save(name, profile);
    return "已向「" + name + "」发送" + set.label + "的" + action_label(action) +
           "红外码。请看一下是否生效，告诉我生效了或没反应（没反应会换下一组）\n（给模型：用户回答后 write device://local/ir/" + name + "/confirm，content 为 yes 或 no）";
}

}  // namespace

String safe_name(String name) {
    name.trim();
    name.replace("/", "_");
    name.replace("..", "_");
    return name;
}

cJSON* load_profile(const String& name) { return load(name); }
bool save_profile(const String& name, cJSON* profile) { return save(name, profile); }

String now_text() {
    const time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char text[20];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &tm);
    return text;
}

void transmit_pulses(const uint16_t* pulses, std::size_t count, uint8_t khz) {
    ensure_started();
    IrSender.sendRaw(pulses, static_cast<uint_fast16_t>(count), khz);
}

String brands_text() {
    String text = "内置红外码（电视）：";
    for (int i = 0; i < kSetCount; ++i) text += String(i ? "、" : "") + kSets[i].label;
    return text + "；动作：电源 power、音量 volume_up / volume_down、静音 mute";
}

String send(const String& name_in, const String& action, const String& brand) {
    const String name = safe_name(name_in);
    if (name.isEmpty()) return "ERROR: 缺少电器名称，例如 device://local/ir/客厅电视/power";
    if (!action_label(action)) return "ERROR: 不支持的动作「" + action + "」。" + brands_text();
    cJSON* profile = load(name);
    int index = -1;
    const cJSON* verified = cJSON_GetObjectItemCaseSensitive(profile, "code_set");
    if (cJSON_IsNumber(verified)) index = verified->valueint;
    if (index < 0 && !brand.isEmpty()) {
        index = set_index(brand);
        if (index < 0) {
            cJSON_Delete(profile);
            return "ERROR: 内置码里没有品牌「" + brand + "」。" + brands_text() + "；可不填品牌逐个尝试";
        }
        put_string(profile, "brand", brand);
    }
    if (index < 0 || index >= kSetCount) index = 0;  // unknown brand: start the round of attempts
    String result;
    if (cJSON_IsNumber(verified)) {
        const CodeSet& set = kSets[index];
        transmit(set.proto, set.address, command_for(set, action));
        put_string(profile, "last_action", action);
        put_string(profile, "last_sent", now_text());
        save(name, profile);
        result = "已向「" + name + "」发送" + action_label(action) + "（" + set.label + "，已验证的码）";
    } else {
        result = attempt(name, profile, index, action);
    }
    cJSON_Delete(profile);
    return result;
}

String confirm(const String& name_in, bool worked) {
    const String name = safe_name(name_in);
    if (!storage::exists(profile_path(name))) return "ERROR: 没有向「" + name + "」发过红外码";
    cJSON* profile = load(name);
    const cJSON* trying = cJSON_GetObjectItemCaseSensitive(profile, "trying");
    const String action = json_str(profile, "pending_action");
    if (!cJSON_IsNumber(trying) || action.isEmpty()) {
        cJSON_Delete(profile);
        return "「" + name + "」没有等待确认的发送";
    }
    const int index = trying->valueint;
    cJSON* history = cJSON_GetObjectItemCaseSensitive(profile, "verifications");
    if (!cJSON_IsArray(history)) history = cJSON_AddArrayToObject(profile, "verifications");
    cJSON* record = cJSON_CreateObject();
    cJSON_AddStringToObject(record, "time", now_text().c_str());
    cJSON_AddStringToObject(record, "code_set", kSets[index].label);
    cJSON_AddStringToObject(record, "action", action.c_str());
    cJSON_AddBoolToObject(record, "worked", worked);
    cJSON_AddItemToArray(history, record);
    while (cJSON_GetArraySize(history) > 20) cJSON_DeleteItemFromArray(history, 0);
    String result;
    if (worked) {
        put_number(profile, "code_set", index);
        put_string(profile, "brand", kSets[index].brand);
        cJSON_DeleteItemFromObject(profile, "trying");
        cJSON_DeleteItemFromObject(profile, "pending_action");
        save(name, profile);
        result = "已记住：「" + name + "」用" + kSets[index].label + "的红外码";
    } else if (index + 1 < kSetCount) {
        cJSON_DeleteItemFromObject(profile, "code_set");
        result = attempt(name, profile, index + 1, action);
    } else {
        cJSON_DeleteItemFromObject(profile, "trying");
        cJSON_DeleteItemFromObject(profile, "pending_action");
        save(name, profile);
        result = "内置的 " + String(kSetCount) + " 组码都试过了，「" + name +
                 "」都没反应。可能品牌不在内置表里，或红外口没对准（在机身侧面，距离 3 米内）";
    }
    cJSON_Delete(profile);
    return result;
}

String send_raw(const String& spec) {
    char proto[12] = {0};
    unsigned address = 0, command = 0;
    if (sscanf(spec.c_str(), "%11s %i %i", proto, &address, &command) != 3) {
        return "ERROR: 格式为「协议 地址 命令」，如 NEC 0x04 0x08；协议支持 NEC / SAMSUNG / SONY / RC5";
    }
    String p = proto;
    p.toUpperCase();
    Proto which;
    if (p == "NEC") which = Proto::Nec;
    else if (p == "SAMSUNG") which = Proto::Samsung;
    else if (p == "SONY") which = Proto::Sony;
    else if (p == "RC5") which = Proto::Rc5;
    else return "ERROR: 协议支持 NEC / SAMSUNG / SONY / RC5";
    transmit(which, address, command);
    return "已发送红外码 " + spec + "，是否生效需要你看一下";
}

String list_text() {
    cJSON* list = cJSON_Parse(storage::list_dir("/vat/things/ir").c_str());
    String text;
    int n = 0;
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
        String file = json_str(entry, "name");
        if (!file.endsWith(".json")) continue;
        const String name = file.substring(0, file.length() - 5);
        cJSON* profile = load(name);
        if (json_str(profile, "kind") == "ac") {
            text += String(++n) + ". " + name + "  " + ac::summary(profile) + "\n";
            cJSON_Delete(profile);
            continue;
        }
        const cJSON* set = cJSON_GetObjectItemCaseSensitive(profile, "code_set");
        text += String(++n) + ". " + name + "  " +
                (cJSON_IsNumber(set) ? String(kSets[set->valueint].label) + "（已验证）" : String("未验证")) +
                (json_str(profile, "last_action").isEmpty() ? "" : "  上次 " + json_str(profile, "last_action") + " " + json_str(profile, "last_sent")) +
                "\n";
        cJSON_Delete(profile);
    }
    cJSON_Delete(list);
    return n ? String(n) + " 个红外电器：\n" + text : "还没有红外电器。" + brands_text();
}

}  // namespace vat::ir
