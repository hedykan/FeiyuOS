#include <functional>
#include "ops_ssh.h"

#include <libssh_esp32.h>
#include <libssh/libssh.h>

#include <map>

#include "arena.h"
#include "common.h"
#include "config.h"
#include "context.h"
#include "net.h"
#include "registry.h"
#include "storage.h"

namespace vat::ssh {
namespace {

const char* const kReadOnlyPrograms[] = {
    "cat", "df", "docker", "free", "git", "head", "hostname", "journalctl", "ls",
    "ps", "pwd", "stat", "systemctl", "tail", "uname", "uptime", "whoami",
};
const char* const kGitSafe[] = {"diff", "grep", "log", "rev-parse", "show", "status"};
const char* const kDockerSafe[] = {"images", "info", "inspect", "logs", "port", "ps", "stats", "top", "version"};
const char* const kSystemctlSafe[] = {"is-active", "is-enabled", "list-unit-files", "list-units", "show", "status"};
const char* const kSensitive[] = {"/.ssh/", "/etc/shadow", ".deepseek_key", ".openroute_voice_key",
                                  ".qwen_key", "secrets.json", "id_ed25519", "id_rsa"};

bool initialised = false;
ssh_session cached = nullptr;
String cached_profile;
std::map<String, bool> connection_flags;

template <std::size_t N>
bool contains(const char* const (&list)[N], const String& value) {
    for (const char* item : list) {
        if (value == item) return true;
    }
    return false;
}

void close_cached() {
    if (cached) {
        ssh_disconnect(cached);
        ssh_free(cached);
        cached = nullptr;
        net::restore_reserve();
    }
    cached_profile = "";
}

ssh_session open_session(const SshProfile& profile, String& error) {
    if (cached && cached_profile == profile.id && ssh_is_connected(cached)) return cached;
    close_cached();
    // The TLS reserve stays put: freeing it here let it come back in the middle
    // of the largest free block after the session (67 -> 32 KB, 2026-11-08).
    ssh_session session = ssh_new();
    if (!session) {
        error = "SSH 内存不足";
        net::restore_reserve();
        return nullptr;
    }
    int port = profile.port;
    long timeout = 10;
    int verbosity = SSH_LOG_NOLOG;
    ssh_options_set(session, SSH_OPTIONS_HOST, profile.host.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, profile.user.c_str());
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    const unsigned long started = millis();
    if (ssh_connect(session) != SSH_OK) {
        error = String("SSH 连接失败: ") + ssh_get_error(session);
        ssh_free(session);
        net::restore_reserve();
        return nullptr;
    }
    cancel::set_socket(ssh_get_fd(session));
    int auth = SSH_AUTH_DENIED;
    if (!profile.key_path.isEmpty()) {
        const String key_text = storage::read_text(profile.key_path, 8192);
        ssh_key key = nullptr;
        if (key_text.isEmpty() ||
            ssh_pki_import_privkey_base64(key_text.c_str(), nullptr, nullptr, nullptr, &key) != SSH_OK) {
            error = "无法读取 SSH 私钥 " + profile.key_path;
        } else {
            auth = ssh_userauth_publickey(session, nullptr, key);
            ssh_key_free(key);
        }
    }
    if (auth != SSH_AUTH_SUCCESS && !profile.password.isEmpty()) {
        auth = ssh_userauth_password(session, nullptr, profile.password.c_str());
    }
    cancel::clear_socket();
    if (auth != SSH_AUTH_SUCCESS) {
        if (error.isEmpty()) error = String("SSH 认证失败: ") + ssh_get_error(session);
        ssh_disconnect(session);
        ssh_free(session);
        net::restore_reserve();
        return nullptr;
    }
    runtime_event("ssh_open", profile.id + " " + String(millis() - started) + "ms");
    cached = session;
    cached_profile = profile.id;
    connection_flags[profile.id] = true;
    return session;
}

}  // namespace

String ExecResult::message() const {
    if (!error.isEmpty()) return error;
    String detail = err;
    detail.trim();
    if (detail.isEmpty()) detail = "exit " + String(exit_code);
    return "SSH 失败: " + utf8_head(detail, 240);
}

void begin() {
    if (!initialised) {
        libssh_begin();
        initialised = true;
    }
}

namespace {

constexpr int kRaw = 1;         // bytes as received: no trailing-newline trim, no truncation note
constexpr int kKeepRemote = 2;  // leave a long stdout on the remote for fetch_output()
constexpr std::size_t kInlineOutput = 4096;
constexpr std::size_t kFullOutputMax = 262144;    // kept on the card
constexpr std::size_t kRemoteReadMax = 1048576;   // read: never copy more than this into the remote temp file

// `sink`, when given, takes stdout as it arrives (false stops the read)
// instead of collecting it in result.out.
using Sink = std::function<bool(const char* data, std::size_t n)>;
ExecResult run_profile(const SshProfile* profile, const String& command, const String* stdin_data,
                       uint32_t timeout_ms, std::size_t max_output, int flags = 0, const Sink* sink = nullptr);

}  // namespace

ExecResult run(const String& profile_id, const String& command, const String* stdin_data,
               uint32_t timeout_ms, std::size_t max_output) {
    const SshProfile* profile = config::find_profile(profile_id);
    if (!profile) {
        ExecResult result;
        result.error = "未知 SSH profile: " + profile_id;
        return result;
    }
    return run_profile(profile, command, stdin_data, timeout_ms, max_output);
}

namespace {

ExecResult run_profile(const SshProfile* profile, const String& command, const String* stdin_data,
                       uint32_t timeout_ms, std::size_t max_output, int flags, const Sink* sink) {
    ExecResult result;
    if (cancel::requested()) {
        result.error = "已取消";
        return result;
    }
    begin();
    ssh_session session = open_session(*profile, result.error);
    if (!session) return result;

    ssh_channel channel = ssh_channel_new(session);
    if (!channel || ssh_channel_open_session(channel) != SSH_OK) {
        result.error = String("SSH 通道失败: ") + ssh_get_error(session);
        if (channel) ssh_channel_free(channel);
        close_cached();
        return result;
    }
    cancel::set_socket(ssh_get_fd(session));
    // The remote side buffers the command's output and returns at most
    // max_output bytes.  This keeps every SSH packet small: libssh decrypts
    // each packet into one contiguous allocation (up to 32 KB by protocol),
    // which a fragmented heap without PSRAM cannot reliably provide.
    // With kKeepRemote a longer stdout stays in $f and its size and path go
    // out first on stderr ("VATOUT <bytes> <path>").
    const String keep =
        (flags & kKeepRemote)
            ? "n=$(wc -c <\"$f\" | tr -d ' '); if [ \"$n\" -gt " + String(max_output) +
                  " ]; then printf 'VATOUT %s %s\\n' \"$n\" \"$f\" >&2; else rm -f \"$f\"; fi; "
            : String("rm -f \"$f\"; ");
    const String wrapped =
        "f=$(mktemp) && e=$(mktemp) || exit 125\n(" + command + "\n) >\"$f\" 2>\"$e\"; rc=$?\nhead -c " +
        String(max_output + 1) + " \"$f\"; " + keep + "head -c 1024 \"$e\" >&2; rm -f \"$e\"; exit $rc";
    if (ssh_channel_request_exec(channel, ("sh -c " + shell_quote(wrapped)).c_str()) != SSH_OK) {
        result.error = String("SSH 执行失败: ") + ssh_get_error(session);
        ssh_channel_free(channel);
        cancel::clear_socket();
        close_cached();
        return result;
    }
    if (stdin_data && !stdin_data->isEmpty()) {
        ssh_channel_write(channel, stdin_data->c_str(), stdin_data->length());
    }
    ssh_channel_send_eof(channel);

    char buffer[512];
    const unsigned long deadline = millis() + timeout_ms;
    bool timed_out = false;
    while (true) {
        if (cancel::requested()) {
            result.error = "已取消";
            break;
        }
        if (millis() > deadline) {
            timed_out = true;
            break;
        }
        const int n = ssh_channel_read_timeout(channel, buffer, sizeof(buffer), 0, 200);
        if (n > 0) {
            if (!sink) {
                result.out.concat(buffer, n);
            } else if (!(*sink)(buffer, n)) {
                result.error = "写入失败";
                break;
            }
            continue;
        }
        const int e = ssh_channel_read_nonblocking(channel, buffer, sizeof(buffer), 1);
        if (e > 0) {
            if (result.err.length() < 2048) result.err.concat(buffer, e);
            continue;
        }
        if (n == SSH_ERROR || e == SSH_ERROR) {
            result.error = String("SSH 读取失败: ") + ssh_get_error(session);
            break;
        }
        if (ssh_channel_is_eof(channel)) break;
    }
    if (result.error.isEmpty() && !timed_out) {
        // Drain any trailing stderr, then collect the exit status.
        int e;
        while ((e = ssh_channel_read_nonblocking(channel, buffer, sizeof(buffer), 1)) > 0) {
            if (result.err.length() < 2048) result.err.concat(buffer, e);
        }
        ssh_channel_close(channel);
        result.exit_code = ssh_channel_get_exit_status(channel);
    }
    ssh_channel_free(channel);
    cancel::clear_socket();
    if (timed_out) {
        result.error = "SSH 操作超时";
        close_cached();
    } else if (!result.error.isEmpty()) {
        close_cached();
    }
    if (result.err.startsWith("VATOUT ")) {
        const int line_end = result.err.indexOf('\n');
        const String line = result.err.substring(7, line_end < 0 ? result.err.length() : line_end);
        const int space = line.indexOf(' ');
        result.total_bytes = line.substring(0, space).toInt();
        result.remote_file = line.substring(space + 1);
        result.err = line_end < 0 ? String() : result.err.substring(line_end + 1);
    }
    if (!(flags & kRaw)) {
        if (result.out.length() > max_output) {
            result.out = utf8_head(result.out, max_output) + "\n…（输出已截断）";
        }
        while (result.out.endsWith("\n") || result.out.endsWith("\r")) result.out.remove(result.out.length() - 1);
    }
    result.ok = result.error.isEmpty() && result.exit_code == 0;
    return result;
}

}  // namespace

void release() { close_cached(); }

ExecResult run_keeping(const String& profile_id, const String& command, const String* stdin_data,
                       uint32_t timeout_ms) {
    const SshProfile* profile = config::find_profile(profile_id);
    if (!profile) {
        ExecResult result;
        result.error = "未知 SSH profile: " + profile_id;
        return result;
    }
    return run_profile(profile, command, stdin_data, timeout_ms, kInlineOutput, kKeepRemote);
}

bool fetch_output(const String& profile_id, const ExecResult& kept, const String& local_path, std::size_t max_bytes) {
    const SshProfile* profile = config::find_profile(profile_id);
    if (!profile || kept.remote_file.isEmpty()) return false;
    storage::remove(local_path);
    const std::size_t wanted = std::min(kept.total_bytes, max_bytes);
    // One command, written to the card as it arrives: no page of it is ever
    // collected in memory (it used to be 12 KB Strings grown chunk by chunk).
    bool ok = storage::ensure_parent(local_path);
    fs::File file = ok ? storage::fs().open(local_path, FILE_WRITE) : fs::File();
    ok = ok && static_cast<bool>(file);
    std::size_t written = 0;
    if (ok) {
        const Sink sink = [&](const char* data, std::size_t n) {
            const std::size_t take = std::min(n, wanted - written);
            if (take && file.write(reinterpret_cast<const uint8_t*>(data), take) != take) return false;
            written += take;
            return true;
        };
        const auto page = run_profile(profile, "head -c " + String(wanted) + " " + shell_quote(kept.remote_file), nullptr,
                                      15000 + wanted / 20, wanted, kRaw, &sink);
        ok = page.error.isEmpty() && written == wanted;
        file.close();
        storage::changed(local_path);  // written through fs()
    }
    run_profile(profile, "rm -f " + shell_quote(kept.remote_file), nullptr, 8000, 64);
    if (ok && kept.total_bytes > wanted) {
        storage::append(local_path, "\n…（输出超过 " + String(wanted) + " 字节，只保存了前 " + String(wanted) + " 字节）\n");
    }
    if (!ok) storage::remove(local_path);
    return ok;
}

namespace {
// A kept output becomes a filed out#N: fetch it to the cache, offer it to the
// context store; the agent files it with the result of this call.
void file_full_output(const String& profile, const ExecResult& result) {
    if (result.remote_file.isEmpty()) return;
    constexpr char kCache[] = "/vat/cache/full.txt";
    const unsigned long started = millis();
    if (fetch_output(profile, result, kCache, kFullOutputMax)) {
        context::offer_full_output(kCache, storage::size(kCache));
    }
    runtime_event("ssh_full_output", String(result.total_bytes) + " bytes " + String(millis() - started) + "ms");
}
}  // namespace

String connect(const String& profile) {
    if (!config::find_profile(profile)) return "未知 SSH profile: " + profile;
    const auto result = run(profile, "true", nullptr, 12000);
    if (!result.ok) {
        connection_flags[profile] = false;
        return "SSH 连接失败: " + result.message();
    }
    connection_flags[profile] = true;
    return "已连接 " + profile;
}

String disconnect(const String& profile) {
    if (!config::find_profile(profile)) return "未知 SSH profile: " + profile;
    if (!connected(profile)) return profile + " 当前未连接";
    if (cached_profile == profile) close_cached();
    connection_flags[profile] = false;
    return "已断开 " + profile;
}

bool connected(const String& profile) {
    const auto it = connection_flags.find(profile);
    return it != connection_flags.end() && it->second;
}

String states_text() {
    String text;
    int n = 0;
    for (const auto& profile : config::get().profiles) {
        text += String(++n) + ". " + profile.id + (connected(profile.id) ? "  已连接" : "  未连接") + "\n";
    }
    return text.isEmpty() ? String("没有 SSH profile") : text;
}

bool parse_target(const String& target, String& profile, String& path, String& error) {
    if (!target.startsWith("ssh://")) {
        error = "无效 SSH target: " + target;
        return false;
    }
    const String rest = target.substring(6);
    const int slash = rest.indexOf('/');
    profile = slash >= 0 ? rest.substring(0, slash) : rest;
    path = slash >= 0 ? rest.substring(slash) : String();
    if (profile.isEmpty()) {
        error = "SSH target 缺少 profile";
        return false;
    }
    if (!config::find_profile(profile)) {
        error = "未知 SSH profile: " + profile;
        return false;
    }
    return true;
}

bool is_sensitive(const String& value) {
    for (const char* marker : kSensitive) {
        if (value.indexOf(marker) >= 0) return true;
    }
    return false;
}

bool is_read_only(const String& program, const std::vector<String>& args) {
    if (!contains(kReadOnlyPrograms, program)) return false;
    for (const auto& arg : args) {
        if (is_sensitive(arg)) return false;
    }
    if (program == "git") return !args.empty() && contains(kGitSafe, args[0]);
    if (program == "docker") return !args.empty() && contains(kDockerSafe, args[0]);
    if (program == "systemctl") return !args.empty() && contains(kSystemctlSafe, args[0]);
    return true;
}

String tool_read(const cJSON* arguments) {
    String profile, path, error;
    if (!parse_target(json_str(arguments, "target"), profile, path, error)) return error;
    if (!path.startsWith("/") || path == "/") return "远端文件必须是明确的绝对路径";
    if (is_sensitive(path)) return "已拒绝：拒绝读取凭据或敏感认证文件";
    // With a card the whole file (up to 256 KB) is kept as out#N; the
    // conversation sees its head.  Without one, the first 4 KB as before.
    // Reading one byte past the store limit tells a cut file from one that fits.
    const long cap = context::on_card() ? static_cast<long>(kRemoteReadMax) : 4096;
    const long limit = constrain(json_int(arguments, "max_bytes", cap), 1, cap);
    const String command = "head -c " + String(limit) + " -- " + shell_quote(path);
    const auto result = context::on_card() ? run_keeping(profile, command) : run(profile, command);
    if (!result.ok) return result.message();
    file_full_output(profile, result);
    return result.out.isEmpty() ? String("文件为空") : result.out;
}

String tool_exec(const cJSON* arguments, bool approved, const String& secret) {
    String profile, path, error;
    const String target = json_str(arguments, "target");
    if (!parse_target(target, profile, path, error)) return error;
    const String program = json_str(arguments, "program");
    if (program.isEmpty()) return "exec 缺少 program";
    std::vector<String> argv{program};
    const cJSON* args = cJSON_GetObjectItemCaseSensitive(arguments, "args");
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, args) {
        if (cJSON_IsString(item)) argv.emplace_back(item->valuestring);
        else if (cJSON_IsNumber(item)) argv.emplace_back(String(item->valuedouble, 0));
    }
    const std::vector<String> rest(argv.begin() + 1, argv.end());
    if (!approved && !is_read_only(program, rest)) {
        return "命令不在自动只读允许范围: " + program;
    }
    String command = shell_join(argv);
    String password_line;
    if (program == "sudo" && !secret.isEmpty()) {
        // Password on stdin only: never on the command line, in logs or results.
        std::vector<String> with_stdin{"sudo", "-S", "-p", ""};
        with_stdin.insert(with_stdin.end(), argv.begin() + 1, argv.end());
        command = shell_join(with_stdin);
        password_line = secret + "\n";
    }
    const String cwd = json_str(arguments, "cwd");
    if (!cwd.isEmpty()) {
        command = "sh -c " + shell_quote("cd -- " + shell_quote(cwd) + " && exec " + command);
    }
    const String* stdin_data = password_line.isEmpty() ? nullptr : &password_line;
    const auto result = context::on_card() ? run_keeping(profile, command, stdin_data, 20000)
                                           : run(profile, command, stdin_data, 20000);
    password_line = String();
    if (registry::data_wanted()) {  // for a program: what the command printed, what it complained about, and its exit code
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "stdout", result.out.c_str());
        cJSON_AddStringToObject(data, "stderr", result.err.c_str());
        cJSON_AddNumberToObject(data, "code", result.exit_code);
        registry::offer_data(data);
    }
    if (!result.ok) {
        if (!result.remote_file.isEmpty()) run(profile, "rm -f " + shell_quote(result.remote_file), nullptr, 8000, 64);
        if (program == "sudo" && result.err.indexOf("incorrect password") >= 0) return "sudo 密码不正确，未执行";
        return result.message();
    }
    file_full_output(profile, result);
    return result.out.isEmpty() ? String("命令已执行，无输出") : result.out;
}

String tool_write(const cJSON* arguments) {
    String profile, path, error;
    if (!parse_target(json_str(arguments, "target"), profile, path, error)) return error;
    if (!path.startsWith("/") || path == "/") return "远端文件必须是明确的绝对路径";
    if (is_sensitive(path)) return "已拒绝：拒绝写入凭据或敏感认证文件";
    const String mode = json_str(arguments, "mode", "overwrite");
    const String content = json_str(arguments, "content");
    String script;
    if (mode == "append") {
        script = "cat >> " + shell_quote(path);
    } else if (mode == "create") {
        script = "set -C && cat > " + shell_quote(path);
    } else {
        script = "cat > " + shell_quote(path);
    }
    const auto result = run(profile, "sh -c " + shell_quote(script), &content, 15000);
    if (!result.ok) return result.message();
    return "已写入 " + profile + ":" + path + "（" + String(content.length()) + " 字节，" + mode + "）";
}

ExecResult run_with_password(const String& profile_id, const String& password, const String& command,
                             const String* stdin_data) {
    const SshProfile* stored = config::find_profile(profile_id);
    ExecResult result;
    if (!stored) {
        result.error = "未知 SSH profile: " + profile_id;
        return result;
    }
    SshProfile once = *stored;  // password for this connection only
    once.id = stored->id + "#password";
    once.key_path = "";
    once.password = password;
    close_cached();
    result = run_profile(&once, command, stdin_data, 15000, 4096);
    close_cached();  // never keep a password-authenticated session around
    once.password = String();
    return result;
}

bool key_accepted(const String& profile_id) { return run(profile_id, "true", nullptr, 12000).ok; }

String device_public_key(String& error) {
    const String path = "/vat/ssh/id_ed25519";
    ssh_key key = nullptr;
    const String existing = storage::read_text(path, 8192);
    if (!existing.isEmpty()) {
        if (ssh_pki_import_privkey_base64(existing.c_str(), nullptr, nullptr, nullptr, &key) != SSH_OK) {
            error = "设备私钥无法读取";
            return {};
        }
    } else {
        if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) {
            error = "无法生成设备密钥";
            return {};
        }
        char* b64 = nullptr;
        if (ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &b64) != SSH_OK ||
            !storage::write_text(path, String(b64))) {
            if (b64) ssh_string_free_char(b64);
            ssh_key_free(key);
            error = "无法保存设备密钥";
            return {};
        }
        ssh_string_free_char(b64);
        runtime_event("ssh_keygen", "ed25519");
    }
    ssh_key pub = nullptr;
    char* b64 = nullptr;
    String line;
    if (ssh_pki_export_privkey_to_pubkey(key, &pub) == SSH_OK && ssh_pki_export_pubkey_base64(pub, &b64) == SSH_OK) {
        line = String(ssh_key_type_to_char(ssh_key_type(pub))) + " " + b64 + " vat-cardputer";
    } else {
        error = "无法导出设备公钥";
    }
    if (b64) ssh_string_free_char(b64);
    ssh_key_free(pub);
    ssh_key_free(key);
    return line;
}

String install_key(const String& profile_id, const String& password) {
    String error;
    const String pub = device_public_key(error);
    if (pub.isEmpty()) return error;
    // The key arrives on stdin; the remote script never sees it on a command line.
    const String script =
        "umask 077; K=$(cat); mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys && "
        "(grep -qxF \"$K\" ~/.ssh/authorized_keys || printf '%s\\n' \"$K\" >> ~/.ssh/authorized_keys)";
    const auto result = run_with_password(profile_id, password, "sh -c " + shell_quote(script), &pub);
    if (!result.ok) {
        return result.message().indexOf("认证失败") >= 0 ? String("密码不正确，未配置免密登录") : result.message();
    }
    // Switch the profile to key authentication and drop any stored password.
    cJSON* root = cJSON_Parse(storage::read_text("/vat/ssh/profiles.json", 32768).c_str());
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "profiles")) {
        if (json_str(item, "id") != profile_id) continue;
        cJSON_DeleteItemFromObject(item, "auth");
        cJSON* auth = cJSON_AddObjectToObject(item, "auth");
        cJSON_AddStringToObject(auth, "type", "private_key");
        cJSON_AddStringToObject(auth, "private_key", "/vat/ssh/id_ed25519");
    }
    char* printed = root ? cJSON_Print(root) : nullptr;
    cJSON_Delete(root);
    if (printed) {
        storage::write_text("/vat/ssh/profiles.json", String(printed));
        cJSON_free(printed);
        config::load();
    }
    if (!key_accepted(profile_id)) return "公钥已写入，但免密登录验证失败";
    connection_flags[profile_id] = true;
    runtime_event("ssh_key_installed", profile_id);
    return "已为 " + profile_id + " 配置免密登录并连接";
}

bool sudo_needs_password(const String& profile_id) {
    const auto result = run(profile_id, "sudo -n true", nullptr, 8000);
    return !result.ok;
}

bool program_available(const String& profile, const String& program) {
    for (std::size_t i = 0; i < program.length(); ++i) {
        const char c = program[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '+' || c == '-')) {
            return false;
        }
    }
    const auto result = run(profile, "command -v -- " + shell_quote(program), nullptr, 6000);
    return result.ok && !result.out.isEmpty();
}

String command_help(const String& profile, const String& program, const String& subcommand) {
    std::vector<String> argv{program};
    if (!subcommand.isEmpty()) argv.push_back(subcommand);
    for (const char* flag : {"--help", "-h"}) {
        auto with_flag = argv;
        with_flag.emplace_back(flag);
        const auto result = run(profile, shell_join(with_flag), nullptr, 6000);
        if (!result.ok || result.out.isEmpty()) continue;
        String cleaned;
        cleaned.reserve(result.out.length());
        for (std::size_t i = 0; i < result.out.length(); ++i) {
            if (result.out[i] == 0x1b) {  // strip ANSI escapes
                ++i;
                if (i < result.out.length() && result.out[i] == '[') {
                    while (i + 1 < result.out.length() &&
                           !(result.out[i + 1] >= '@' && result.out[i + 1] <= '~')) ++i;
                    ++i;
                }
                continue;
            }
            cleaned += result.out[i];
        }
        return utf8_head(cleaned, 4000);
    }
    return "无法取得 " + program + " " + subcommand + " 的帮助";
}

}  // namespace vat::ssh
