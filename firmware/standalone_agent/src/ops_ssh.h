#pragma once

#include <Arduino.h>
#include <cJSON.h>

#include <vector>

namespace vat::ssh {

struct ExecResult {
    bool ok = false;        // transport + exit status 0
    int exit_code = -1;
    String out;
    String err;
    String error;           // transport/auth problem
    std::size_t total_bytes = 0;  // whole stdout size when it was left on the remote (see run_keeping)
    String remote_file;           // where it was left
    String message() const; // human readable failure
};

void begin();  // libssh global init, once after Wi-Fi

// Runs one command on the profile.  The SSH session is cached for the
// duration of the current worker job and closed by release().
ExecResult run(const String& profile, const String& command,
               const String* stdin_data = nullptr, uint32_t timeout_ms = 15000,
               std::size_t max_output = 4096);
void release();
// Like run(), but when stdout is longer than 4 KB the whole output is left in
// a remote temp file (total_bytes/remote_file) for fetch_output().
ExecResult run_keeping(const String& profile, const String& command, const String* stdin_data = nullptr,
                       uint32_t timeout_ms = 20000);
// Copies up to max_bytes of a kept remote output into a local file, 12 KB per
// SSH round trip (every packet stays small), then deletes the remote file.
bool fetch_output(const String& profile, const ExecResult& kept, const String& local_path, std::size_t max_bytes);

// Connection bookkeeping used by session(connect/disconnect/list).
String connect(const String& profile);
String disconnect(const String& profile);
String states_text();  // numbered "1. 开发机  已连接"
bool connected(const String& profile);

// Target helpers: ssh://PROFILE[/ABSOLUTE_PATH]
bool parse_target(const String& target, String& profile, String& path, String& error);

bool is_sensitive(const String& value);
bool is_read_only(const String& program, const std::vector<String>& args);

// Tool executors.  They return the text shown to the user; failures are
// reported as text too so the agent loop never throws.
String tool_read(const cJSON* arguments);
String tool_exec(const cJSON* arguments, bool approved, const String& secret = String());

// One connection authenticated by a password typed on the device; nothing is
// cached or stored.  Used to install the device key and for sudo checks.
ExecResult run_with_password(const String& profile, const String& password, const String& command,
                             const String* stdin_data = nullptr);
bool key_accepted(const String& profile);
// Generates /vat/ssh/id_ed25519 on first use; returns "ssh-ed25519 AAAA… vat-cardputer".
String device_public_key(String& error);
// Appends the device key to ~/.ssh/authorized_keys using the password once,
// then switches the profile to key authentication.
String install_key(const String& profile, const String& password);
bool sudo_needs_password(const String& profile);
String tool_write(const cJSON* arguments);
bool program_available(const String& profile, const String& program);
String command_help(const String& profile, const String& program, const String& subcommand);

}  // namespace vat::ssh
