// Host tests for the program engine (prog_engine.cpp): validation, the
// interpreter, waits across save/restore, program calls.  Included by
// host_test.cpp; uses its CHECK macro.
#include <map>
#include <string>

#include "prog_engine.h"

namespace {

using vat::prog::Result;
using vat::prog::Run;
using vat::prog::Status;

// Records every call as "tool target|content"; answers by target prefix.
struct FakeHost : vat::prog::Host {
    std::map<std::string, std::string> programs;
    std::vector<std::pair<std::string, std::string>> answers;  // target prefix -> result
    std::vector<std::string> failing;                          // target prefixes that fail
    std::vector<std::string> calls;
    int slice = -1;  // calls left before the time slice ends (-1: never)

    bool call(const String&, const String& tool, const cJSON* args, const cJSON*, String& out) override {
        const std::string target = vat::json_str(args, "target").c_str();
        const std::string content = vat::json_str(args, "content").c_str();
        calls.push_back(std::string(tool.c_str()) + " " + target + (content.empty() ? "" : "|" + content));
        if (slice > 0) --slice;
        for (const auto& f : failing) {
            if (target.rfind(f, 0) == 0) {
                out = "boom";
                return false;
            }
        }
        out = "ok";
        for (const auto& a : answers) {
            if (target.rfind(a.first, 0) == 0) out = a.second.c_str();
        }
        return true;
    }
    cJSON* load(const String& name) override {
        const auto it = programs.find(name.c_str());
        return it == programs.end() ? nullptr : cJSON_Parse(it->second.c_str());
    }
    String now() override { return "07:00"; }
    bool out_of_time() override { return slice == 0; }
};

String check(const char* json) {
    cJSON* p = cJSON_Parse(json);
    vat::prog::Info info;
    const String err = p ? vat::prog::validate(p, info) : String("parse");
    cJSON_Delete(p);
    return err;
}

// Runs `name` to the end, saving and restoring at every wait.
Result run_through(FakeHost& host, const char* name, const char* query = "", const char* event = nullptr) {
    cJSON* ev = event ? cJSON_Parse(event) : nullptr;
    auto run = std::make_unique<Run>();
    String err = run->start(host, name, query, ev);
    cJSON_Delete(ev);
    if (!err.isEmpty()) {
        Result r;
        r.status = Status::Failed;
        r.error = err;
        return r;
    }
    for (int guard = 0; guard < 50; ++guard) {
        const Result r = run->resume(host);
        if (r.status != Status::Waiting && r.status != Status::Yielded) return r;
        const String state = run->save();
        run = std::make_unique<Run>();  // a fresh job picks it up
        err = run->restore(host, state);
        if (!err.isEmpty()) {
            Result f;
            f.status = Status::Failed;
            f.error = err;
            return f;
        }
    }
    return Result{};
}

bool called(const FakeHost& host, const std::string& line) {
    for (const auto& c : host.calls) {
        if (c == line) return true;
    }
    return false;
}

void prog_validation() {
    CHECK(check(R"({"name":"a","steps":[{"write":"device://local/led","content":"red"}]})").isEmpty());
    CHECK(has(check(R"({"name":"a","when":"x","steps":[]})"), "不认识的字段"));
    CHECK(has(check(R"({"name":"a","steps":[]})"), "非空"));
    CHECK(has(check(R"({"name":"a/b","steps":[{"wait":"1m"}]})"), "name"));
    CHECK(has(check(R"({"name":"a","steps":[{"read":"x://1","write":"y://2"}]})"), "有且只有一个"));
    CHECK(has(check(R"({"name":"a","steps":[{"write":"x://{w}"}]})"), "没有用 as 定义"));
    CHECK(check(R"({"name":"a","steps":[{"read":"x://1","as":"w"},{"write":"y://{w}"}]})").isEmpty());
    CHECK(has(check(R"({"name":"a","steps":[{"wait":"forever"}]})"), "wait"));
    CHECK(has(check(R"({"name":"a","steps":[{"wait":"25h"}]})"), "wait"));
    CHECK(has(check(R"({"name":"a","steps":[{"repeat":11,"steps":[{"wait":"1s"}]}]})"), "repeat"));
    CHECK(has(check(R"({"name":"a","steps":[{"read":"x://1","as":"r"},{"if":"r","then":[{"wait":"1s"}]}]})"),
              "比较"));
    CHECK(has(check(R"({"name":"a","input":{"to":null},"steps":[{"read":"x://{input.from}"}]})"), "没有在 input"));
    CHECK(has(check(R"({"name":"a","steps":[{"read":"x://1","on_error":"maybe"}]})"), "on_error"));
    // JSON or prose inside a content string is not a variable.
    CHECK(check(R"({"name":"a","steps":[{"write":"x://1","content":"{\"k\": 1} {a b}"}]})").isEmpty());
    String many = R"({"name":"a","steps":[)";
    for (int i = 0; i < 41; ++i) many += String(i ? "," : "") + R"({"wait":"1s"})";
    many += "]}";
    CHECK(has(check(many.c_str()), "超过 40"));

    cJSON* p = cJSON_Parse(R"({"name":"a","steps":[{"read":"ask://q","as":"r"},{"wait":"1m"},
        {"exec":"prog://b?x={r}","as":"y"},{"write":"z://{input.to}"}]})");
    vat::prog::Info info;
    CHECK(vat::prog::validate(p, info).isEmpty());
    CHECK(info.uses_ask && info.uses_wait && info.callees.size() == 1 && info.callees[0] == "b");
    CHECK(info.inputs.size() == 1 && info.inputs[0].name == "to" && info.inputs[0].required);
    cJSON_Delete(p);
}

void prog_stream() {
    // Validation: a stream needs `as`, a fixed target, and {a.x} only after it.
    CHECK(has(check(R"({"name":"a","steps":[{"stream":"sensor://imu/acceleration"}]})"), "要写 as"));
    CHECK(has(check(R"({"name":"a","steps":[{"read":"x://1","as":"t"},{"stream":"sensor://{t}","as":"a"}]})"), "不能用变量"));
    CHECK(has(check(R"({"name":"a","steps":[{"write":"x://{a.x}"}]})"), "没有用 as 定义"));
    CHECK(check(R"({"name":"a","steps":[{"stream":"sensor://imu/acceleration","as":"a"},{"write":"x://{a.x}"}]})").isEmpty());

    // Run: the value is a JSON object; {a.x} reads a field, if compares it.
    FakeHost host;
    host.programs["倾斜"] = R"({"name":"倾斜","steps":[
        {"stream":"sensor://imu/acceleration","as":"a"},
        {"if":"a.z","above":"0.9","then":[{"write":"device://local/led","content":"green"}],
                                  "else":[{"write":"device://local/led","content":"red"}]},
        {"write":"device://local/display/text","content":"x={a.x} y={a.y} w={a.w}"}]})";
    host.answers = {{"sensor://imu/acceleration", R"({"x":0.1,"y":-0.25,"z":0.98})"}};
    Result r = run_through(host, "倾斜");
    CHECK(r.status == Status::Done);
    CHECK(called(host, "stream sensor://imu/acceleration"));
    CHECK(called(host, "write device://local/led|green"));
    CHECK(called(host, "write device://local/display/text|x=0.1 y=-0.25 w="));
}

void prog_run_basic() {
    FakeHost host;
    host.programs["早安"] = R"({"name":"早安","steps":[
        {"write":"device://local/audio/beep","content":"3"},
        {"read":"place://weather","as":"weather"},
        {"write":"ask://","content":"天气是 {weather}，会下雨吗？","as":"rain"},
        {"if":"rain","is":"是","then":[{"write":"device://local/display/banner","content":"带伞 {now}"}],
                              "else":[{"write":"device://local/led","content":"green"}]},
        {"write":"ask://","content":"早安：{weather}"}]})";
    host.answers = {{"place://weather", "小雨 21℃"}, {"ask://", "是"}};
    Result r = run_through(host, "早安");
    CHECK(r.status == Status::Done);
    CHECK(host.calls.size() == 5);
    CHECK(called(host, "write ask://|天气是 小雨 21℃，会下雨吗？"));
    CHECK(called(host, "write device://local/display/banner|带伞 07:00"));
    CHECK(!called(host, "write device://local/led|green"));

    FakeHost dry;
    dry.programs = host.programs;
    dry.answers = {{"place://weather", "晴"}, {"ask://", "否"}};
    r = run_through(dry, "早安");
    CHECK(r.status == Status::Done && called(dry, "write device://local/led|green"));
    CHECK(!called(dry, "write device://local/display/banner|带伞 07:00"));
}

void prog_run_waits() {
    FakeHost host;
    host.programs["番茄钟"] = R"({"name":"番茄钟","steps":[
        {"repeat":3,"steps":[{"write":"device://local/led","content":"red"},{"wait":"25m"},
                             {"write":"device://local/led","content":"green"},{"wait":"5m"}]},
        {"write":"device://local/display/banner","content":"完成"}]})";
    auto run = std::make_unique<Run>();
    CHECK(run->start(host, "番茄钟", "", nullptr).isEmpty());
    Result r = run->resume(host);
    CHECK(r.status == Status::Waiting && r.wait_s == 1500);
    r = run_through(host, "番茄钟");  // a separate run through every save/restore
    CHECK(r.status == Status::Done);
    int red = 0;
    for (const auto& c : host.calls) red += c == "write device://local/led|red";
    CHECK(red == 1 + 3);  // the first partial run, then the full one
    CHECK(host.calls.back() == "write device://local/display/banner|完成");

    // A time slice ends: the run yields and carries on afterwards.
    FakeHost sliced;
    sliced.programs["三步"] = R"({"name":"三步","steps":[{"write":"a://1"},{"write":"a://2"},{"write":"a://3"}]})";
    sliced.slice = 1;
    auto s = std::make_unique<Run>();
    CHECK(s->start(sliced, "三步", "", nullptr).isEmpty());
    r = s->resume(sliced);
    CHECK(r.status == Status::Yielded && sliced.calls.size() == 1);
    sliced.slice = -1;
    r = s->resume(sliced);
    CHECK(r.status == Status::Done && sliced.calls.size() == 3);
}

void prog_run_calls() {
    FakeHost host;
    host.programs["走去"] = R"({"name":"走去","input":{"to":null,"mode":"walk"},"steps":[
        {"read":"place://route?to={input.to}&mode={input.mode}","as":"r"},
        {"write":"device://local/display/text","content":"{r}"}]})";
    host.programs["午饭"] = R"({"name":"午饭","steps":[
        {"read":"place://nearby?q=餐厅","as":"list"},
        {"exec":"prog://走去?to={list}","as":"shown"},
        {"write":"device://local/display/banner","content":"已显示：{shown}"}]})";
    host.answers = {{"place://nearby", "美宜佳&赛格店"}, {"place://route", "向南 51 米"},
                    {"device://local/display/text", "显示了"}};
    Result r = run_through(host, "午饭");
    CHECK(r.status == Status::Done);
    // The value went into the query without breaking it (& removed), default mode applied.
    CHECK(called(host, "read place://route?to=美宜佳 赛格店&mode=walk"));
    CHECK(called(host, "write device://local/display/banner|已显示：显示了"));  // callee's last result returned

    host.calls.clear();
    r = run_through(host, "走去", "to=%E5%85%AC%E5%9B%AD&mode=bike");
    CHECK(r.status == Status::Done && called(host, "read place://route?to=公园&mode=bike"));

    r = run_through(host, "走去");  // required parameter missing
    CHECK(r.status == Status::Failed && has(r.error, "缺少参数 to"));

    host.programs["容错"] = R"({"name":"容错","steps":[
        {"exec":"prog://不存在","on_error":"continue"},
        {"exec":"prog://坏的","on_error":"continue"},
        {"write":"device://local/display/banner","content":"错误：{error}"}]})";
    host.programs["坏的"] = R"({"name":"坏的","steps":[{"write":"fail://x"}]})";
    host.failing = {"fail://"};
    host.calls.clear();
    r = run_through(host, "容错");
    CHECK(r.status == Status::Done);
    CHECK(host.calls.back().find("错误：") != std::string::npos && host.calls.back().find("boom") != std::string::npos);

    host.programs["严格"] = R"({"name":"严格","steps":[{"write":"fail://y"},{"write":"never://z"}]})";
    host.calls.clear();
    r = run_through(host, "严格");
    CHECK(r.status == Status::Failed && has(r.error, "boom") && !called(host, "write never://z"));

    // Call graph checks at save time.
    host.programs["A"] = R"({"name":"A","steps":[{"exec":"prog://B"}]})";
    host.programs["B"] = R"({"name":"B","steps":[{"exec":"prog://A"}]})";
    CHECK(has(vat::prog::check_calls("A", {"B"}, host), "成环"));
    host.programs["C"] = R"({"name":"C","steps":[{"exec":"prog://D"}]})";
    host.programs["D"] = R"({"name":"D","steps":[{"exec":"prog://E"}]})";
    host.programs["E"] = R"({"name":"E","steps":[{"write":"a://1"}]})";
    CHECK(has(vat::prog::check_calls("X", {"C"}, host), "层数"));
    CHECK(vat::prog::check_calls("X", {"D"}, host).isEmpty());
    CHECK(has(vat::prog::check_calls("X", {"没有"}, host), "不存在"));
}

void prog_run_limits() {
    FakeHost host;
    host.programs["多"] = R"({"name":"多","steps":[{"repeat":10,"steps":[{"repeat":10,"steps":[
        {"write":"a://1"},{"write":"a://2"}]}]}]})";
    const Result r = run_through(host, "多");
    CHECK(r.status == Status::Failed && has(r.error, "上限"));

    FakeHost ev;
    ev.programs["开门"] = R"({"name":"开门","steps":[{"if":"event.text","has":"开门",
        "then":[{"write":"device://local/display/banner","content":"收到：{event.text}"}]}]})";
    const Result e = run_through(ev, "开门", "", R"({"text":"请开门 谢谢"})");
    CHECK(e.status == Status::Done && called(ev, "write device://local/display/banner|收到：请开门 谢谢"));
}

void prog_helpers() {
    CHECK(vat::prog::parse_wait("25m") == 1500);
    CHECK(vat::prog::parse_wait("1h30m") == 5400);
    CHECK(vat::prog::parse_wait("90s") == 90);
    CHECK(vat::prog::parse_wait("10") == -1 && vat::prog::parse_wait("") == -1 && vat::prog::parse_wait("5x") == -1);
    String name, query;
    CHECK(vat::prog::program_target("prog://走去?to=x", name, query) && name == "走去" && query == "to=x");
    CHECK(!vat::prog::program_target("ssh://dev", name, query));

    cJSON* a = cJSON_Parse(R"({"write":"ir://空调/power","content":"on","as":"x"})");
    cJSON* b = cJSON_Parse(R"({"write":"ir://空调/power","content":"on"})");
    cJSON* c = cJSON_Parse(R"({"write":"ir://空调/power","content":"off"})");
    CHECK(vat::prog::fingerprint(a) == vat::prog::fingerprint(b));  // as does not change what runs
    CHECK(vat::prog::fingerprint(b) != vat::prog::fingerprint(c));
    String tool;
    cJSON* args = vat::prog::action_arguments(b, tool);
    CHECK(tool == "write" && vat::json_str(args, "target") == "ir://空调/power");
    CHECK(vat::json_str(args, "mode") == "overwrite" && !vat::json_bool(args, "confirm_required", true));
    cJSON_Delete(args);
    cJSON_Delete(a);
    cJSON_Delete(b);
    cJSON_Delete(c);
}

void prog_engine() {
    prog_validation();
    prog_stream();
    prog_run_basic();
    prog_run_waits();
    prog_run_calls();
    prog_run_limits();
    prog_helpers();
}

}  // namespace
