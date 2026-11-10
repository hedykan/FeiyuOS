// card: the packages on the TF card (/vat/things/lua/名字.lua with @package
// and @target), enabled once through pkg://.  A call to a card package's
// target runs its file in a Lua VM and calls the global function named like
// the tool: read(target, args), write(target, args) or exec(target, args);
// what it returns is the result (a table as JSON), error() an ERROR.  Its own
// vat calls are allowed by its confirmed @uses, like a program's.
//
// One registered package stands for all of them: the registry's list never
// changes after boot (the UI task walks it), so this one keeps its own list,
// reloaded when the programs' index or confirmations change.  A target a
// built-in package already serves is skipped (built-ins come first anyway).
#include <mutex>

#include "../common.h"
#include "../prog_runtime.h"
#include "../registry.h"

namespace vat::registry {
namespace {

constexpr std::size_t kUsageEach = 240;   // bytes of one package's usage in the prompt
constexpr std::size_t kUsageTotal = 1200;

// usage() goes into the JSON system prompt as is: no quotes, backslashes or
// control characters.
String prompt_safe(const String& text) {
    String out;
    out.reserve(text.length());
    for (std::size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (c == '"') out += "”";
        else if (c == '\\') out += '/';
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

class CardPackages : public Package {
public:
    const Manifest& manifest() const override {
        refresh();
        return manifest_;
    }

    const char* usage() const override {
        refresh();
        return usage_.c_str();
    }

    bool supports(const String& tool, const cJSON* arguments) const override {
        if (tool != "read" && tool != "write" && tool != "exec") return false;
        return !owner(json_str(arguments, "target")).isEmpty();
    }

    String execute(const String& tool, const cJSON* arguments, bool) override {
        const String name = owner(json_str(arguments, "target"));
        if (name.isEmpty()) return "ERROR: 没有扩展包负责这个目标";
        return programs::run_package(name, tool, arguments);
    }

    void resources(cJSON* list, int& remaining) override {
        refresh();
        std::lock_guard<std::mutex> guard(lock_);
        for (const auto& p : list_) {
            if (!add_resource(list, remaining, p.target.c_str(), "card")) break;
        }
    }

private:
    String owner(const String& target) const {
        refresh();
        std::lock_guard<std::mutex> guard(lock_);
        for (const auto& p : list_) {
            if (target.startsWith(p.target)) return p.name;
        }
        return {};
    }

    bool built_in(const String& target) const {
        cJSON* probe = cJSON_CreateObject();
        cJSON_AddStringToObject(probe, "target", (target + "x").c_str());
        bool taken = false;
        for (auto* package : packages()) {
            if (package == this) continue;
            for (const char* tool : {"read", "write", "exec"}) taken = taken || package->supports(tool, probe);
            if (taken) break;
        }
        cJSON_Delete(probe);
        return taken;
    }

    void refresh() const {
        const uint32_t now = programs::generation();
        std::lock_guard<std::mutex> guard(lock_);
        if (loaded_ && now == generation_) return;
        loaded_ = true;
        generation_ = now;
        list_.clear();
        usage_ = String();
        keywords_ = "扩展包 卡上";
        summary_ = String();
        for (auto& p : programs::card_packages()) {
            bool clash = built_in(p.target);
            for (const auto& other : list_) clash = clash || p.target.startsWith(other.target) || other.target.startsWith(p.target);
            if (clash) {
                runtime_event("card_package_skipped", p.name + " " + p.target);
                continue;
            }
            if (!p.usage.isEmpty() && usage_.length() < kUsageTotal) {
                usage_ += "扩展包「" + prompt_safe(p.name) + "」（" + prompt_safe(p.target) + "）：" +
                          prompt_safe(utf8_head(p.usage, kUsageEach)) + "。";
            }
            keywords_ += " " + p.name + " " + p.target + " " + p.keywords;
            summary_ += String(summary_.isEmpty() ? "" : "、") + p.name + " " + p.target +
                        (p.about.isEmpty() ? String() : "（" + p.about + "）");
            list_.push_back(p);
        }
        if (summary_.isEmpty()) summary_ = "（还没有启用的）";
        summary_ = "卡上的扩展包：" + summary_;
        manifest_.summary = summary_.c_str();
        manifest_.keywords = keywords_.c_str();
    }

    mutable std::mutex lock_;
    mutable bool loaded_ = false;
    mutable uint32_t generation_ = 0;
    mutable std::vector<programs::PackageInfo> list_;
    mutable String usage_;
    mutable String keywords_;
    mutable String summary_;
    mutable Manifest manifest_{"card", "卡上的扩展包", "扩展包", {}};
};

}  // namespace

Package* make_card_package() { return new CardPackages(); }

}  // namespace vat::registry
