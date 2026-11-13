#include "ops_tui.h"

#include "common.h"
#include "config.h"
#include "ops_ssh.h"
#include "storage.h"

namespace vat::tui {
namespace {

Session active;
bool active_valid = false;

String discovery_script(const String& provider) {
    return "command -v " + provider +
           " || find ~/.config/nvm/versions/node ~/.local/bin ~/.npm-global/bin -path '*/bin/" + provider +
           "' 2>/dev/null | sort -V | tail -1";
}

bool resolve_session(const String& target, Session& session, String& error) {
    if (target.isEmpty() || target == "tui://current" || target == "agent://current") {
        if (!active_valid) {
            // A failure (registry::failed): repeating the call cannot help, and the agent stops after two.
            error = "ERROR: 当前没有活动 TUI，请先启动 Codex、Pi 或连接 tmux 会话";
            return false;
        }
        session = active;
        return true;
    }
    if (active_valid && target == active.target()) {
        session = active;
        return true;
    }
    String rest;
    if (target.startsWith("tui://")) rest = target.substring(6);
    else if (target.startsWith("tmux://")) rest = target.substring(7);
    const int slash = rest.indexOf('/');
    if (slash <= 0 || slash == static_cast<int>(rest.length()) - 1) {
        error = "未知 TUI target: " + target;
        return false;
    }
    session.profile = rest.substring(0, slash);
    session.name = rest.substring(slash + 1);
    session.provider = "tmux";
    if (!config::find_profile(session.profile)) {
        error = "未知 SSH profile: " + session.profile;
        return false;
    }
    return true;
}

String project_path(const String& project, const String& profile_id, String& error) {
    String value = project;
    value.trim();
    if (value.isEmpty()) {
        error = "启动远端 Agent 需要 project";
        return {};
    }
    if (value.startsWith("/")) return value;
    for (std::size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) {
            error = "project 名称只能包含字母、数字、点、横线和下划线";
            return {};
        }
    }
    const SshProfile* profile = config::find_profile(profile_id);
    String root = profile && !profile->project_root.isEmpty() ? profile->project_root : String("~");
    if (root.endsWith("/")) root.remove(root.length() - 1);
    return root + "/" + value;
}

bool session_exists(const String& profile, const String& name) {
    return ssh::run(profile, "tmux has-session -t " + shell_quote(name), nullptr, 6000).ok;
}

String map_key(const String& key) {
    String k = key;
    k.toLowerCase();
    if (k == "enter") return "Enter";
    if (k == "tab") return "Tab";
    if (k == "esc" || k == "escape") return "Escape";
    if (k == "ctrl_c" || k == "c-c" || k == "control_c") return "C-c";
    if (k == "ctrl_d" || k == "c-d") return "C-d";
    if (k == "up") return "Up";
    if (k == "down") return "Down";
    if (k == "left") return "Left";
    if (k == "right") return "Right";
    if (k == "backspace" || k == "bspace") return "BSpace";
    if (k == "y" || k == "n") return k;
    return {};
}

void remember_active() {
    cJSON* record = cJSON_CreateObject();
    cJSON_AddStringToObject(record, "profile", active.profile.c_str());
    cJSON_AddStringToObject(record, "name", active.name.c_str());
    cJSON_AddStringToObject(record, "provider", active.provider.c_str());
    cJSON_AddStringToObject(record, "project", active.project.c_str());
    storage::write_text("/vat/memory/tui.json", json_print(record));
    cJSON_Delete(record);
}

}  // namespace

int term_columns = 39;
int term_rows = 9;
String sized;  // "name@profile CxR" last applied to a vat-* session

String size_args() { return " -x " + String(term_columns) + " -y " + String(term_rows) + " "; }

void set_size(int columns, int rows) {
    term_columns = columns;
    term_rows = rows;
}

String ensure_shell(const String& profile) {
    if (profile.isEmpty() || !config::find_profile(profile)) return "未知 SSH profile: " + profile;
    const String name = "vat-shell";
    const String command = "tmux has-session -t " + name + " 2>/dev/null || tmux new-session -d -s " + name +
                           size_args() + shell_quote("env PS1='$ ' bash --noprofile --norc");
    const auto result = ssh::run(profile, "sh -c " + shell_quote(command), nullptr, 10000);
    if (!result.ok) {
        if (result.exit_code == 127 || result.err.indexOf("not found") >= 0) {
            return "远端没有安装 tmux，SSH 页终端不可用（远程命令不受影响）";
        }
        return "终端会话不可用：" + result.message();
    }
    active = {profile, name, "shell", "~"};
    active_valid = true;
    remember_active();
    return {};
}

bool has_current() {
    if (!active_valid) {
        // Restore the last session after a reboot; it is validated lazily.
        cJSON* record = cJSON_Parse(storage::read_text("/vat/memory/tui.json", 1024).c_str());
        if (record) {
            active.profile = json_str(record, "profile");
            active.name = json_str(record, "name");
            active.provider = json_str(record, "provider");
            active.project = json_str(record, "project");
            active_valid = !active.profile.isEmpty() && !active.name.isEmpty();
            cJSON_Delete(record);
        }
    }
    return active_valid;
}

Session current() { return active; }

String start_agent(const String& provider, const String& project, const String& prompt,
                   const String& profile) {
    if (provider != "codex" && provider != "pi" && provider != "claude") {
        return "暂不支持远程 Agent: " + provider;
    }
    if (!config::find_profile(profile)) return "未知 SSH profile: " + profile;
    String error;
    const String path_project = project_path(project, profile, error);
    if (!error.isEmpty()) return error;
    const auto found = ssh::run(profile, "sh -lc " + shell_quote(discovery_script(provider)), nullptr, 10000);
    if (!found.ok && found.out.isEmpty()) return "连不上 " + profile + "，没能查找 " + provider + "：" + found.message();
    String program = found.out;
    const int last_newline = program.lastIndexOf('\n');
    if (last_newline >= 0) program = program.substring(last_newline + 1);
    program.trim();
    if (!program.startsWith("/")) return "远端未找到 " + provider;
    // Resolve ~ in the project path remotely and verify it exists.
    const auto dir = ssh::run(profile, "sh -c " + shell_quote("cd " + path_project + " && pwd"), nullptr, 6000);
    if (!dir.ok || dir.out.isEmpty()) return "远端项目不存在: " + path_project;
    const String resolved = dir.out;

    String slug = project;
    slug.trim();
    if (slug.endsWith("/")) slug.remove(slug.length() - 1);
    slug = slug.substring(slug.lastIndexOf('/') + 1);
    for (std::size_t i = 0; i < slug.length(); ++i) {
        if (!(isalnum(static_cast<unsigned char>(slug[i])) || slug[i] == '_' || slug[i] == '-')) slug.setCharAt(i, '-');
    }
    String name = utf8_head("vat-" + provider + "-" + slug, 48);

    std::vector<String> argv{program};
    if (provider == "codex") {
        argv.insert(argv.end(), {"--sandbox", "read-only", "-a", "never", "--no-alt-screen"});
    } else if (provider == "pi") {
        argv.insert(argv.end(), {"--tools", "read,grep,find,ls", "--no-session", "--no-context-files",
                                 "--no-approve", "--tui-mode", "regular"});
    } else {
        argv.insert(argv.end(), {"--permission-mode", "plan"});
    }
    const String command =
        "tmux kill-session -t " + shell_quote(name) + " 2>/dev/null; tmux new-session -d -s " +
        shell_quote(name) + " -x 100 -y 30 -c " + shell_quote(resolved) + " " +
        shell_quote(shell_join(argv));
    const auto started = ssh::run(profile, "sh -c " + shell_quote(command), nullptr, 12000);
    if (!started.ok) return "启动失败: " + started.message();
    active = {profile, name, provider, resolved};
    active_valid = true;
    remember_active();
    String result = "已启动只读 " + provider + " TUI：" + active.target();
    String trimmed = prompt;
    trimmed.trim();
    if (!trimmed.isEmpty()) {
        // Give the TUI time to draw its input box before pasting the task.
        delay(provider == "codex" ? 3500 : 2500);
        const String sent = send_input("tui://current", trimmed, {}, true);
        result += "；" + sent;
    }
    return result;
}

String observe(const String& target, std::size_t maximum) {
    Session session;
    String error;
    if (!resolve_session(target, session, error)) return error;
    // Sessions the device made follow the SSH page's font size; resizing in
    // the same command costs no extra round trip.
    String command = "tmux capture-pane -p -J -t " + shell_quote(session.name) + " -S -100";
    const String want = session.name + "@" + session.profile + " " + String(term_columns) + "x" + String(term_rows);
    const bool resize = session.name.startsWith("vat-") && sized != want;
    if (resize) {
        command = "tmux resize-window -t " + shell_quote(session.name) + size_args() + "2>/dev/null; " + command;
    }
    const auto result = ssh::run(session.profile, command, nullptr, 8000, 6000);
    if (resize && result.ok) sized = want;
    if (!result.ok) {
        const String detail = result.err + result.error;
        if (detail.indexOf("can't find") >= 0 || detail.indexOf("no server running") >= 0 ||
            detail.indexOf("session not found") >= 0) {
            // The remote session is gone (closed, killed, host rebooted).
            if (active_valid && session.name == active.name && session.profile == active.profile) {
                active_valid = false;
                storage::remove("/vat/memory/tui.json");
            }
            return kSessionGone;
        }
        return result.message();
    }
    String output = result.out;
    while (output.endsWith("\n") || output.endsWith(" ")) output.remove(output.length() - 1);
    if (output.isEmpty()) return "TUI 当前没有可见输出";
    return utf8_tail(output, maximum);
}

String compact_screen(const String& screen, std::size_t maximum_lines) {
    maximum_lines = std::max<std::size_t>(maximum_lines, 11);
    std::vector<String> kept;
    bool blank = false;
    int start = 0;
    while (start <= static_cast<int>(screen.length())) {
        int end = screen.indexOf('\n', start);
        if (end < 0) end = screen.length();
        String line = screen.substring(start, end);
        start = end + 1;
        line.trim();
        if (line.isEmpty()) {
            if (!kept.empty() && !blank) kept.push_back("");
            blank = true;
            continue;
        }
        blank = false;
        bool rule = line.length() >= 12;
        for (std::size_t i = 0; rule && i < line.length();) {
            if (line.startsWith("─", i) || line.startsWith("━", i) || line.startsWith("═", i)) i += 3;
            else if (line[i] == '-' || line[i] == ' ') ++i;
            else rule = false;
        }
        if (rule) continue;
        // Powerline/zsh theme decorations from users' own shells.
        if (line.indexOf("░▒▓") >= 0 || line.indexOf("▓▒░") >= 0 || line.indexOf("\xEE\x82\xB0") >= 0 /* U+E0B0 powerline arrow */) continue;
        String folded = line;
        folded.toLowerCase();
        if (folded.indexOf("working") >= 0 && (line.indexOf("─") >= 0 || line.indexOf("━") >= 0)) {
            kept.push_back("Working...");
            continue;
        }
        if (folded.startsWith("update available") || folded.startsWith("new version ") ||
            folded.startsWith("changelog:") || folded.startsWith("openai codex") ||
            line.startsWith("~/") || (line.indexOf("/262k") >= 0 && line.indexOf("$") >= 0) ||
            line == "Ask Codex to do anything" ||
            line == "Pi can explain its own features and look up its docs.") {
            continue;
        }
        kept.push_back(line);
    }
    while (!kept.empty() && kept.back().isEmpty()) kept.pop_back();
    String result;
    const std::size_t first = kept.size() > maximum_lines ? kept.size() - maximum_lines : 0;
    for (std::size_t i = first; i < kept.size(); ++i) {
        if (!result.isEmpty()) result += "\n";
        result += kept[i];
    }
    return result.isEmpty() ? String("TUI 当前没有可见输出") : result;
}

String send_input(const String& target, const String& text, const std::vector<String>& keys,
                  bool submit) {
    Session session;
    String error;
    if (!resolve_session(target, session, error)) return error;
    const String quoted = shell_quote(session.name);
    String script;
    if (!text.isEmpty()) {
        script += "tmux set-buffer -- " + shell_quote(text) + " && tmux paste-buffer -d -t " + quoted +
                  " && sleep 0.4";
    }
    for (const auto& key : keys) {
        const String mapped = map_key(key);
        if (mapped.isEmpty()) return "不支持的 TUI key: " + key;
        if (!script.isEmpty()) script += " && ";
        script += "tmux send-keys -t " + quoted + " " + mapped;
    }
    if (submit) {
        if (!script.isEmpty()) script += " && ";
        script += "tmux send-keys -t " + quoted + " Enter";
    }
    if (script.isEmpty()) return "没有要发送的内容";
    const auto result = ssh::run(session.profile, "sh -c " + shell_quote(script), nullptr, 10000);
    if (!result.ok) return result.message();
    return "已发送到当前 TUI";
}

String interrupt(const String& target) {
    const String result = send_input(target, "", {"esc"}, false);
    return result == "已发送到当前 TUI" ? String("已向当前 TUI 发送 Esc") : result;
}

void forget(const String& profile) {
    if (has_current() && active.profile == profile) {
        active_valid = false;
        storage::remove("/vat/memory/tui.json");
    }
}

String close(const String& target) {
    Session session;
    String error;
    if (!resolve_session(target, session, error)) return error;
    ssh::run(session.profile, "tmux kill-session -t " + shell_quote(session.name), nullptr, 6000);
    if (active_valid && session.name == active.name && session.profile == active.profile) {
        active_valid = false;
        storage::remove("/vat/memory/tui.json");
    }
    return "TUI 会话已关闭";
}

String tmux_list(const String& target) {
    String profile = target.startsWith("tmux://") ? target.substring(7) : target;
    const int slash = profile.indexOf('/');
    if (slash >= 0) profile = profile.substring(0, slash);
    if (!config::find_profile(profile)) return "未知 SSH profile: " + profile;
    const auto result =
        ssh::run(profile, "tmux list-sessions -F '#{session_name} #{session_windows}'", nullptr, 6000);
    if (!result.ok) {
        return result.err.indexOf("no server") >= 0
                   ? "没有 tmux 会话（要终端就 session connect ssh://" + profile + "，会自动新建 vat-shell 会话）"
                   : result.message();
    }
    String text;
    int n = 0;
    int start = 0;
    const String& out = result.out;
    while (start < static_cast<int>(out.length())) {
        int end = out.indexOf('\n', start);
        if (end < 0) end = out.length();
        String line = out.substring(start, end);
        start = end + 1;
        line.trim();
        if (line.isEmpty()) continue;
        const int space = line.lastIndexOf(' ');
        const String name = space > 0 ? line.substring(0, space) : line;
        const String windows = space > 0 ? line.substring(space + 1) : String("1");
        text += String(++n) + ". " + name + "（" + windows + " 个窗口" +
                (has_current() && active.profile == profile && active.name == name ? "，当前" : "") + "）\n";
    }
    return text.isEmpty() ? "没有 tmux 会话（要终端就 session connect ssh://" + profile + "，会自动新建 vat-shell 会话）"
                          : profile + " 上的 tmux 会话：\n" + text;
}

String tmux_create(const String& target) {
    Session session;
    String error;
    if (!resolve_session(target, session, error)) return error;
    // A bare shell with a one-character prompt reads well on a 240x135 screen.
    const auto result = ssh::run(session.profile,
                                 "tmux new-session -d -s " + shell_quote(session.name) + size_args() +
                                     shell_quote("env PS1='$ ' bash --noprofile --norc"),
                                 nullptr, 8000);
    if (!result.ok && result.err.indexOf("duplicate") < 0) return result.message();
    active = session;
    active_valid = true;
    remember_active();
    return "已创建并切换到 tmux 会话 " + session.name;
}

String tmux_attach(const String& target) {
    Session session;
    String error;
    if (!resolve_session(target, session, error)) return error;
    if (!session_exists(session.profile, session.name)) return "ERROR: tmux 会话不存在: " + session.name;
    active = session;
    active_valid = true;
    remember_active();
    return "已连接 tmux 会话 " + session.name + "，可在 SSH 页查看";
}

String tool_observe(const cJSON* arguments) {
    const String target = json_str(arguments, "target", "tui://current");
    return observe(target);
}

String tool_input(const cJSON* arguments) {
    std::vector<String> keys;
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(arguments, "keys");
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        if (cJSON_IsString(item)) keys.emplace_back(item->valuestring);
    }
    return send_input(json_str(arguments, "target", "tui://current"), json_str(arguments, "text"), keys,
                      json_bool(arguments, "submit", false));
}

}  // namespace vat::tui
