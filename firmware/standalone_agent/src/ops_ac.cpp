#include "ops_ac.h"

#include <functional>

#include "ac_core.h"
#include "arena.h"
#include "common.h"
#include "ops_ir.h"
#include "storage.h"

namespace vat::ac {
namespace {

constexpr std::size_t kChunk = 2048;  // index lines are under 700 bytes
constexpr uint8_t kCarrierKhz = 38;

const char kMissing[] = "TF 卡上没有空调红外码库 /vat/ir/ac.irdb：从 GitHub Release 下载 ac.irdb，放到 TF 卡的 /vat/ir/ 目录";

// Calls `visit` for each brand line until it returns true.  `blob` is where
// the code files start.  false: no database (error says so).
bool scan(const std::function<bool(const Brand&)>& visit, std::size_t& blob, String& error) {
    const String head = storage::read_range(kDatabase, 0, 64);
    const int newline = head.indexOf('\n');
    const long length = newline < 0 ? -1 : index_length(head.substring(0, newline));
    if (length <= 0) {
        error = storage::exists(kDatabase) ? String("空调码库文件格式不对：") + kDatabase : String(kMissing);
        return false;
    }
    const std::size_t start = newline + 1;
    blob = start + static_cast<std::size_t>(length);
    String carry;
    for (std::size_t at = start; at < blob; at += kChunk) {
        String text = carry + storage::read_range(kDatabase, at, std::min(kChunk, blob - at));
        int from = 0, end;
        while ((end = text.indexOf('\n', from)) >= 0) {
            Brand brand;
            if (parse_brand(text.substring(from, end), brand) && visit(brand)) return true;
            from = end + 1;
        }
        carry = text.substring(from);
    }
    return true;
}

// The first brand (most common first) the words name; `blob` as in scan.
bool find_brand(const String& words, Brand& out, std::size_t& blob, String& error) {
    bool found = false;
    const auto visit = [&](const Brand& b) {
        if (!brand_matches(b, words)) return false;
        out = b;
        found = true;
        return true;
    };
    if (!scan(visit, blob, error)) return false;
    if (!found) error = "码库里没有空调品牌「" + words + "」。" + brands_text();
    return found;
}

// Encodes `state` with `remote` and transmits it.
bool transmit(const Remote& remote, std::size_t blob, State& state, uint8_t key, String& error) {
    std::vector<uint8_t> code;
    if (!storage::read_range_bytes(kDatabase, blob + remote.offset, remote.length, code)) {
        error = "读不到红外码（" + remote.id + "）";
        return false;
    }
    const arena::Hold buffer("ir_decode", 4096 + remote.length);
    if (!buffer.ok()) {
        error = "内存不够，稍后再试";
        return false;
    }
    std::vector<uint16_t> pulses;
    if (!encode(code, state, key, pulses, error)) return false;
    ir::transmit_pulses(pulses.data(), pulses.size(), kCarrierKhz);
    return true;
}

int remote_index(const Brand& brand, const String& id) {
    for (std::size_t i = 0; i < brand.remotes.size(); ++i) {
        if (brand.remotes[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

String which(const Brand& brand, int i) {
    return brand.name + "第 " + String(i + 1) + "/" + String(static_cast<int>(brand.remotes.size())) + " 套码";
}

void record(cJSON* profile, const State& state, const String& words) {
    cJSON* object = json_obj(profile, "state");
    if (!object) object = cJSON_AddObjectToObject(profile, "state");
    to_json(state, object);
    cJSON_DeleteItemFromObject(profile, "last_action");
    cJSON_AddStringToObject(profile, "last_action", words.c_str());
    cJSON_DeleteItemFromObject(profile, "last_sent");
    cJSON_AddStringToObject(profile, "last_sent", ir::now_text().c_str());
}

void set_number(cJSON* profile, const char* key, double value) {
    cJSON_DeleteItemFromObject(profile, key);
    cJSON_AddNumberToObject(profile, key, value);
}

String ask_text(const String& name) {
    return "。请看空调有没有响应（滴一声或开始运转），告诉我生效了或没反应（没反应会换下一套；也可以说上一套、下一套或第几套）\n"
           "（给模型：用户回答后 write device://local/ir/" + name + "/confirm，content 为 yes 或 no；要换到指定的码用 write device://local/ir/" + name +
           "/try，content 为 上一套、下一套、重发 或编号数字）";
}

// From remote `from`, moving by `step` (+1 / -1, 0: that one only), the
// first one that can encode the profile's state; it is sent.  -1 when none.
int send_from(cJSON* profile, const Brand& brand, std::size_t blob, int from, int step, State& state, String& error) {
    const uint8_t key = static_cast<uint8_t>(json_int(profile, "pending_key", 0));
    const int count = static_cast<int>(brand.remotes.size());
    for (int i = from; i >= 0 && i < count; i += step) {
        state = from_json(json_obj(profile, "state"));
        if (transmit(brand.remotes[i], blob, state, key, error)) return i;
        if (step == 0) break;
    }
    return -1;
}

}  // namespace

bool is_ac(const String& name) {
    cJSON* profile = ir::load_profile(name);
    const bool ac = json_str(profile, "kind") == "ac";
    cJSON_Delete(profile);
    return ac;
}

String brands_text() {
    String names;
    int n = 0;
    std::size_t blob = 0;
    String error;
    if (!scan([&](const Brand& b) { names += String(n ? "、" : "") + b.name; return ++n >= 20; }, blob, error)) {
        return error;
    }
    return "码库里的空调品牌（常见在前）：" + names + (n >= 20 ? " 等" : "");
}

String send(const String& name_in, const String& words) {
    const String name = ir::safe_name(name_in);
    if (name.isEmpty()) return "ERROR: 缺少电器名称，例如 device://local/ir/客厅空调/ac";
    cJSON* profile = ir::load_profile(name);
    if (json_str(profile, "kind") != "ac" && cJSON_GetObjectItemCaseSensitive(profile, "code_set")) {
        cJSON_Delete(profile);
        return "ERROR: 「" + name + "」已经记为电视，空调换个名字";
    }
    const State before = from_json(json_obj(profile, "state"));
    State after = before;
    String brand_words, error;
    if (!apply(words, after, brand_words, error)) {
        cJSON_Delete(profile);
        return "ERROR: " + error + "。写法如「开 制冷 26 自动风」「关」「+1」「除湿」「高风」，首次加品牌";
    }
    const String brand_name = json_str(profile, "brand");
    if (brand_name.isEmpty() && brand_words.isEmpty()) {
        cJSON_Delete(profile);
        return "ERROR: 第一次控制「" + name + "」要说空调品牌（如 奥克斯 开 26）。" + brands_text();
    }
    Brand brand;
    std::size_t blob = 0;
    if (!find_brand(brand_words.isEmpty() ? brand_name : brand_words, brand, blob, error)) {
        cJSON_Delete(profile);
        return "ERROR: " + error;
    }
    if (brand.name != brand_name) {  // a new or different brand: start its remotes over
        for (const char* key : {"remote", "trying", "pending_key"}) cJSON_DeleteItemFromObject(profile, key);
    }
    cJSON_DeleteItemFromObject(profile, "name");
    cJSON_AddStringToObject(profile, "name", name.c_str());
    cJSON_DeleteItemFromObject(profile, "kind");
    cJSON_AddStringToObject(profile, "kind", "ac");
    cJSON_DeleteItemFromObject(profile, "brand");
    cJSON_AddStringToObject(profile, "brand", brand.name.c_str());

    int index = remote_index(brand, json_str(profile, "remote"));
    const bool verified = index >= 0;
    if (!verified) {
        cJSON_DeleteItemFromObject(profile, "remote");
        index = static_cast<int>(json_int(profile, "trying", 0));
        if (index < 0 || index >= static_cast<int>(brand.remotes.size())) index = 0;
    }
    const int wanted_temp = after.temp;
    const uint8_t key = key_for(before, after);
    if (!transmit(brand.remotes[index], blob, after, key, error)) {
        cJSON_Delete(profile);
        return "ERROR: " + error;
    }
    record(profile, after, words);
    String result = "已向「" + name + "」发送：" + describe(after);
    if (after.on && after.temp != wanted_temp) result += "（这套码最高 / 最低只到 " + String(after.temp) + "°C）";
    if (verified) {
        result += "（" + brand.name + "，已验证的码）";
    } else {
        set_number(profile, "trying", index);
        set_number(profile, "pending_key", key);
        result += "，用的是" + which(brand, index) + ask_text(name);
    }
    ir::save_profile(name, profile);
    cJSON_Delete(profile);
    return result;
}

String confirm(const String& name_in, bool worked) {
    const String name = ir::safe_name(name_in);
    cJSON* profile = ir::load_profile(name);
    const cJSON* trying = cJSON_GetObjectItemCaseSensitive(profile, "trying");
    if (!cJSON_IsNumber(trying)) {
        cJSON_Delete(profile);
        return "「" + name + "」没有等待确认的发送";
    }
    Brand brand;
    String error;
    std::size_t blob = 0;
    if (!find_brand(json_str(profile, "brand"), brand, blob, error)) {
        cJSON_Delete(profile);
        return "ERROR: " + error;
    }
    const int index = trying->valueint;
    if (index < 0 || index >= static_cast<int>(brand.remotes.size())) {
        cJSON_Delete(profile);
        return "ERROR: 「" + name + "」的试码记录不对，重新发一次";
    }
    cJSON* history = json_arr(profile, "verifications");
    if (!history) history = cJSON_AddArrayToObject(profile, "verifications");
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "time", ir::now_text().c_str());
    cJSON_AddStringToObject(entry, "remote", brand.remotes[index].id.c_str());
    cJSON_AddBoolToObject(entry, "worked", worked);
    cJSON_AddItemToArray(history, entry);
    while (cJSON_GetArraySize(history) > 20) cJSON_DeleteItemFromArray(history, 0);

    String result;
    if (worked) {
        cJSON_DeleteItemFromObject(profile, "remote");
        cJSON_AddStringToObject(profile, "remote", brand.remotes[index].id.c_str());
        cJSON_DeleteItemFromObject(profile, "trying");
        cJSON_DeleteItemFromObject(profile, "pending_key");
        result = "已记住：「" + name + "」用" + which(brand, index) + "（" + brand.remotes[index].id + "）";
    } else {
        // The next remote that can encode the state (one may lack the mode).
        State state;
        const int next = send_from(profile, brand, blob, index + 1, 1, state, error);
        if (next >= 0) {
            set_number(profile, "trying", next);
            record(profile, state, json_str(profile, "last_action"));
            result = "换了" + which(brand, next) + "，又发了一次：" + describe(state) + ask_text(name);
        }
    }
    if (result.isEmpty()) {
        cJSON_DeleteItemFromObject(profile, "trying");
        cJSON_DeleteItemFromObject(profile, "pending_key");
        result = brand.name + "的 " + String(static_cast<int>(brand.remotes.size())) + " 套码都试过了，「" + name +
                 "」都没反应。可能型号比码库新，或红外口没对准（在机身侧面，靠近空调接收窗，3 米内）";
    }
    ir::save_profile(name, profile);
    cJSON_Delete(profile);
    return result;
}

String choose(const String& name_in, const String& words_in) {
    const String name = ir::safe_name(name_in);
    cJSON* profile = ir::load_profile(name);
    if (json_str(profile, "kind") != "ac" || !json_obj(profile, "state")) {
        cJSON_Delete(profile);
        return "ERROR: 「" + name + "」还没控制过，先 write device://local/ir/" + name + "/ac 带上品牌发一次";
    }
    Brand brand;
    String error;
    std::size_t blob = 0;
    if (!find_brand(json_str(profile, "brand"), brand, blob, error)) {
        cJSON_Delete(profile);
        return "ERROR: " + error;
    }
    const int count = static_cast<int>(brand.remotes.size());
    const cJSON* trying = cJSON_GetObjectItemCaseSensitive(profile, "trying");
    int current = cJSON_IsNumber(trying) ? trying->valueint : remote_index(brand, json_str(profile, "remote"));
    if (current < 0 || current >= count) current = 0;
    String words = words_in;
    words.trim();
    int from = current, step = 0;
    if (words.indexOf("上") >= 0 || words.indexOf("前") >= 0 || words == "prev" || words == "previous") {
        from = current - 1;
        step = -1;
    } else if (words.indexOf("下") >= 0 || words.indexOf("后") >= 0 || words == "next") {
        from = current + 1;
        step = 1;
    } else if (words.indexOf("重") < 0 && words != "again") {
        String digits;
        for (unsigned i = 0; i < words.length(); ++i) {
            if (isdigit(static_cast<unsigned char>(words[i]))) digits += words[i];
        }
        if (digits.isEmpty()) {
            cJSON_Delete(profile);
            return "ERROR: content 写 上一套、下一套、重发 或编号数字（1 到 " + String(count) + "）";
        }
        from = digits.toInt() - 1;
        if (from < 0 || from >= count) {
            cJSON_Delete(profile);
            return "ERROR: " + brand.name + "只有 " + String(count) + " 套码，编号要在 1 到 " + String(count) + " 之间";
        }
    }
    if (from < 0 || from >= count) {
        cJSON_Delete(profile);
        return "已经是" + which(brand, current) + "，" + (step < 0 ? "前面" : "后面") + "没有了";
    }
    State state;
    const int index = send_from(profile, brand, blob, from, step, state, error);
    if (index < 0) {
        cJSON_Delete(profile);
        return step == 0 ? "ERROR: " + which(brand, from) + "发不了当前状态：" + error
                         : "ERROR: " + String(step < 0 ? "前面" : "后面") + "的码都发不了当前状态（" + error + "）";
    }
    cJSON_DeleteItemFromObject(profile, "remote");  // chosen by hand: confirm it again
    set_number(profile, "trying", index);
    record(profile, state, json_str(profile, "last_action"));
    ir::save_profile(name, profile);
    cJSON_Delete(profile);
    return "换到" + which(brand, index) + "，发了一次：" + describe(state) + ask_text(name);
}

String summary(const cJSON* profile) {
    const String remote = json_str(profile, "remote");
    return "空调 " + json_str(profile, "brand") + (remote.isEmpty() ? "（未验证）" : "（已验证 " + remote + "）") + "  " +
           describe(from_json(json_obj(profile, "state")));
}

}  // namespace vat::ac
