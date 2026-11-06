#pragma once

#include <Arduino.h>
#include <cJSON.h>

#include <vector>

namespace vat::tui {

struct Session {
    String profile;
    String name;
    String provider;  // codex | pi | claude | tmux
    String project;
    String target() const { return "tui://" + profile + "/" + name; }
};

// Text observe() returns when the remembered remote session no longer exists;
// the session is forgotten at the same time.
constexpr char kSessionGone[] = "TUI 会话已不存在";

bool has_current();
// Attaches to (or creates) the plain shell session "vat-shell" on the profile
// and makes it the SSH page's current session.  Used by session(connect);
// the firmware never opens terminals on its own.  "" on success.
// Terminal size the SSH page shows (columns x rows for its font).  Sessions
// the device creates (vat-*) are created at this size and resized to it on
// the next observe; other sessions are never resized.
void set_size(int columns, int rows);
String ensure_shell(const String& profile);
// Forget the current session if it belongs to the profile (disconnect).
void forget(const String& profile);
Session current();

String start_agent(const String& provider, const String& project, const String& prompt,
                   const String& profile);
String observe(const String& target, std::size_t maximum = 8000);
// The compact feed is what the SSH page shows at detail 0.
String compact_screen(const String& screen, std::size_t maximum_lines = 9);
String send_input(const String& target, const String& text,
                  const std::vector<String>& keys, bool submit);
String interrupt(const String& target);
String close(const String& target);

// tmux://PROFILE[/NAME]
String tmux_list(const String& target);
String tmux_create(const String& target);
String tmux_attach(const String& target);

// Tool executors.
String tool_observe(const cJSON* arguments);
String tool_input(const cJSON* arguments);

}  // namespace vat::tui
