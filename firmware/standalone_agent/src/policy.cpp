#include "policy.h"


#include <map>
#include <vector>

#include "common.h"
#include "ops_ssh.h"
#include "registry.h"

namespace vat::policy {
namespace {

std::map<String, String> target_modes;  // target prefix -> deny|ask|allow|allow_all

// Local reversible preferences do not need a confirmation round trip.
}  // namespace

void set_mode(const String& target, const String& mode) { target_modes[target] = mode; }

String mode_for(const String& target) {
    const auto exact = target_modes.find(target);
    if (exact != target_modes.end()) return exact->second;
    String best;
    String mode = "allow";
    for (const auto& item : target_modes) {
        String prefix = item.first;
        while (prefix.endsWith("/")) prefix.remove(prefix.length() - 1);
        if (target.startsWith(prefix + "/") && item.first.length() > best.length()) {
            best = item.first;
            mode = item.second;
        }
    }
    return mode;
}

bool hard_denied(const cJSON* arguments) { return ssh::is_sensitive(json_print(arguments)); }

Result evaluate(const String& tool, const cJSON* arguments) {
    String target = json_str(arguments, "target");
    if (target.isEmpty()) target = json_str(arguments, "profile");
    const String mode = mode_for(target);
    if (tool == "discover") return {Decision::Allow, "能力查询只读"};
    if (mode == "deny") return {Decision::Deny, "该目标当前策略为 deny"};
    if (hard_denied(arguments)) return {Decision::Deny, "拒绝访问凭据或敏感认证文件"};
    if (tool == "session" && json_str(arguments, "action") == "set_policy") {
        return {Decision::Ask, "策略改变需要显式确认"};
    }
    if (mode == "ask") return {Decision::Ask, "该目标当前策略为 ask"};
    if (tool == "read") return {Decision::Allow, "资源读取"};
    if (tool == "exec") {
        if (registry::reversible(tool, arguments)) return {Decision::Allow, "能力包声明可直接执行（已确认过）"};
        std::vector<String> args;
        const cJSON* list = cJSON_GetObjectItemCaseSensitive(arguments, "args");
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, list) {
            if (cJSON_IsString(item)) args.emplace_back(item->valuestring);
        }
        const bool safe = ssh::is_read_only(json_str(arguments, "program"), args);
        const bool confirm = json_bool(arguments, "confirm_required", false);
        if (safe && !confirm) return {Decision::Allow, "白名单只读命令"};
        if (mode == "allow_all") return {Decision::Allow, "该目标处于临时 allow_all"};
        return {Decision::Ask, "命令会改变状态或不在只读允许表"};
    }
    if (tool == "write") {
        if (mode == "allow_all") return {Decision::Allow, "该目标处于临时 allow_all"};
        if (registry::reversible(tool, arguments)) return {Decision::Allow, "本机可逆设置"};
        return {Decision::Ask, "持久写入需要确认"};
    }
    if (tool == "observe" || tool == "input") return {Decision::Allow, "交互目标操作"};
    if (tool == "session") {
        const String action = json_str(arguments, "action");
        if (action == "approve" || action == "reject") return {Decision::Ask, "审批动作需要显式确认"};
        return {Decision::Allow, "会话操作"};
    }
    return {Decision::Deny, "未知操作"};
}

}  // namespace vat::policy
