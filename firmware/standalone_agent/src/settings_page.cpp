#include "web_token.h"
#include "settings_page.h"

#include <WiFi.h>

#include <functional>
#include <vector>

#include "common.h"
#include "config.h"
#include "net.h"
#include "ops_lora.h"
#include "ops_portal.h"
#include "ops_ssh.h"
#include "ops_wifi.h"
#include "settings_store.h"
#include "storage.h"
#include "ui_kit.h"
#include "ui_state.h"

namespace vat::ui::settings {
namespace {

enum class View : uint8_t { Menu, WifiList, Form, Message, Keys, Hosts, Host, PublicKey, Lora, Font, Storage, Portal, WebToken, Context };
const char* const kViewNames[] = {"menu", "wifi", "form", "message", "keys", "hosts", "host", "pubkey", "lora", "font", "storage", "portal", "webtoken", "context"};

struct Field {
    String label;
    String value;
    bool secret = false;
};

// A short sequence of typed fields: Enter moves on, the last Enter submits.
struct Form {
    String title;
    std::vector<Field> fields;
    int index = 0;
    View back = View::Menu;
    int back_cursor = 0;
    std::function<void()> submit;
};

struct MenuItem {
    const char* label;
    View view;
};
const MenuItem kMenu[] = {{"WiFi", View::WifiList}, {"Key", View::Keys},   {"SSH 主机", View::Hosts},
                          {"LoRa", View::Lora},     {"终端字号", View::Font}, {"对话记忆", View::Context}, {"存储", View::Storage},
                          {"手机设置（扫码）", View::Portal}, {"网页令牌", View::WebToken}};
constexpr int kMenuCount = sizeof(kMenu) / sizeof(kMenu[0]);

struct Provider {
    const char* id;
    const char* label;
};
const Provider kProviders[] = {{"qwen", "千问（语音识别）"}, {"deepseek", "DeepSeek（Agent）"}, {"openrouter", "OpenRouter（备用语音）"}, {"amap", "高德（位置）"}};
constexpr int kProviderCount = 4;
const uint8_t kFontSizes[] = {10, 12, 14};
const uint8_t kContextKb[] = {32, 64, 96};

bool is_open = false;
String reason;
bool boot_offline = false;  // entered at boot without a network
bool dirty = true;
uint32_t last_draw = 0;
View view = View::Menu;
int cursor = 0;
std::vector<wifi::Network> networks;
bool scanned = false;
Form form;
bool reveal = false;
bool caps = false;  // own caps lock: letters only (the library one also shifts digits)
String message;
bool message_ok = false;
View after_message = View::Menu;
int after_cursor = 0;
String host_id;
bool delete_armed = false;
String public_key;
int portal_step = 0;      // 0: join the hotspot, 1: open the page
int portal_clients = -1;  // last count drawn
String portal_notice;     // last notice drawn

const String& provider_key(int i) {
    const auto& cfg = config::get();
    return i == 0 ? cfg.qwen_key : i == 1 ? cfg.deepseek_key : i == 2 ? cfg.openrouter_key : cfg.amap_key;
}

int menu_index(View v) {
    for (int i = 0; i < kMenuCount; ++i) {
        if (kMenu[i].view == v) return i;
    }
    return 0;
}

void go(View v, int at = 0) {
    view = v;
    cursor = at;
    delete_armed = false;
}

void show(const String& text, bool ok, View back, int back_cursor = 0) {
    view = View::Message;
    message = text;
    message_ok = ok;
    after_message = back;
    after_cursor = back_cursor;
}

void open_form(const String& title, std::vector<Field> fields, View back, int back_cursor, std::function<void()> submit) {
    form = Form();
    form.title = title;
    form.fields = std::move(fields);
    form.back = back;
    form.back_cursor = back_cursor;
    form.submit = std::move(submit);
    reveal = false;
    go(View::Form);
}

void clear_form() {
    for (auto& f : form.fields) f.value = String();
    form = Form();
}

// ---------------------------------------------------------------- drawing

void row(Gfx& g, int y, bool selected, const String& left, const String& right, uint16_t color = kText) {
    const uint16_t bg = selected ? kSurface : kBg;
    g.fillRect(0, y - 1, kWidth, kLine, bg);
    if (selected) g.fillRect(0, y - 1, 2, kLine, kAccent);
    g.setTextColor(color, bg);
    g.drawString(fit(g, left, kWidth - kPad * 2 - g.textWidth(right) - 8), kPad, y);
    g.setTextColor(kMuted, bg);
    g.drawRightString(right, kWidth - kPad, y);
}

void footer(Gfx& g, const String& text) {
    g.setTextColor(kMuted, kBg);
    g.drawCentreString(fit(g, text, kWidth - 4), kWidth / 2, kHeight - kLine);
}

constexpr int kTop = kBarHeight + 3;
constexpr int kRows = (kHeight - kTop - kLine) / kLine;

struct Line {
    String left;
    String right;
    uint16_t color = kText;
};

void list(Gfx& g, const std::vector<Line>& lines) {
    const int count = lines.size();
    const int first = std::max(0, std::min(cursor - kRows / 2, count - kRows));
    for (int r = 0; r < kRows && first + r < count; ++r) {
        const auto& l = lines[first + r];
        row(g, kTop + r * kLine, cursor == first + r, l.left, l.right, l.color);
    }
}

void text_block(Gfx& g, const String& text, uint16_t color, int y = kTop + 4) {
    g.setTextColor(color, kBg);
    for (const auto& line : wrap_text(g, text, kWidth - kPad * 2)) {
        if (y > kHeight - kLine * 2) break;
        g.drawString(line, kPad, y);
        y += kLine;
    }
}

String section_title() {
    switch (view) {
        case View::Menu: return "设置";
        case View::Form: return "设置 · " + form.title;
        case View::Message: return "设置";
        case View::Host: return "设置 · " + host_id;
        case View::PublicKey: return "设置 · 设备公钥";
        case View::Context: return "设置 · 对话记忆";
        case View::WebToken: return "设置 · 网页令牌";
        default: return String("设置 · ") + kMenu[menu_index(view)].label;
    }
}

std::vector<Line> lora_lines() {
    const auto& l = config::get().lora;
    const String state = !lora::cap_present() ? String("未检测到 Cap")
                         : !l.enabled                ? String("已关闭")
                         : l.frequency_mhz <= 0      ? String("已开·缺频点")
                                                     : String("已开启");
    return {{"开关", state},
            {"频点", l.frequency_mhz > 0 ? String(l.frequency_mhz, 1) + " MHz" : String("未设置")},
            {"发射功率", String(l.power_dbm) + " dBm"},
            {"天线", l.antenna_confirmed ? String("已确认装好") : String("未确认")}};
}

void draw_page() {
    auto& g = M5Cardputer.Display;
    g.startWrite();
    g.fillScreen(kBg);
    g.setFont(&fonts::efontCN_12);
    g.setTextSize(1);
    g.fillRect(0, 0, kWidth, kBarHeight, kSurface);
    g.setTextColor(kText, kSurface);
    g.drawString(fit(g, section_title(), 150), kPad, 2);
    g.setTextColor(boot_offline ? kWarn : kMuted, kSurface);
    g.drawRightString(boot_offline ? fit(g, reason, 84) : fit(g, net::ssid(), 84), kWidth - kPad, 2);

    switch (view) {
        case View::Menu: {
            const auto& cfg = config::get();
            const String values[] = {
                net::ssid().isEmpty() ? String("未联网") : net::ssid(),
                cfg.deepseek_key.isEmpty() || cfg.qwen_key.isEmpty() ? String("缺 Key") : String("已设置"),
                String(cfg.profiles.size()) + " 台",
                !lora::cap_present() ? String("无 Cap") : !cfg.lora.enabled ? String("关") : cfg.lora.frequency_mhz <= 0 ? String("缺频点") : String("开"),
                String(cfg.terminal_font) + " 号",
                String(cfg.context_kb) + " KB",
                storage::kind() == "tf" ? String("TF 卡") : storage::degraded() ? String("板载（降级）") : String("板载"),
                portal::active() ? String("热点开") : String(""),
                String("")};
            static_assert(sizeof(values) / sizeof(values[0]) == kMenuCount, "one value per menu row");
            std::vector<Line> lines;
            for (int i = 0; i < kMenuCount; ++i) lines.push_back({kMenu[i].label, values[i]});
            list(g, lines);
            if (net::state() == net::WifiState::SetupAp) {
                g.setTextColor(kMuted, kBg);
                g.drawString("热点 " + net::ap_name() + "  http://" + net::ip(), kPad, kTop + kMenuCount * kLine + 2);
            }
            footer(g, boot_offline ? "; . 选择  Enter 进入" : "; . 选择  Enter 进入  ` 退出");
            break;
        }
        case View::WifiList: {
            if (!scanned) {
                g.setTextColor(kMuted, kBg);
                g.drawCentreString("正在搜索附近的 WiFi…", kWidth / 2, 60);
            } else {
                std::vector<Line> lines;
                for (std::size_t i = 0; i < networks.size(); ++i) {
                    const auto& n = networks[i];
                    const bool current = n.ssid == net::ssid();
                    lines.push_back({String(i + 1) + ". " + n.ssid,
                                     String(current ? "当前 " : "") + wifi::signal_word(n.rssi) + (n.secured ? " 加密" : " 开放"),
                                     current ? kAccent : kText});
                }
                lines.push_back({"手动输入网络名（仅 2.4 GHz）…", "", kMuted});
                list(g, lines);
            }
            if (scanned && networks.empty()) {
                footer(g, wifi::last_scan_failed() ? "搜索失败，Fn+R 重搜  ` 返回" : "没搜到 2.4 GHz WiFi  Fn+R 重搜");
            } else {
                footer(g, "; . 选择  Enter 连接  Fn+R 重搜  ` 返回");
            }
            break;
        }
        case View::Form: {
            const Field& f = form.fields[form.index];
            g.setTextColor(kText, kBg);
            String label = f.label;
            if (form.fields.size() > 1) label += "（" + String(form.index + 1) + "/" + String(form.fields.size()) + "）";
            g.drawString(fit(g, label, kWidth - kPad * 2), kPad, kTop + 2);
            g.fillRoundRect(kPad, kTop + kLine + 6, kWidth - kPad * 2, kLine + 6, 3, kSurface);
            String shown;
            if (f.secret && !reveal) {
                for (std::size_t i = 0; i < f.value.length(); ++i) shown += "*";
            } else {
                shown = f.value;
            }
            g.setTextColor(kText, kSurface);
            g.drawString(utf8_tail(shown + "_", 36), kPad + 4, kTop + kLine + 9);
            g.setTextColor(kMuted, kBg);
            String info = "已输入 " + String(f.value.length()) + " 位";
            if (caps) info += "  大写锁定";
            if (f.secret) info += reveal ? "  明文" : "  隐藏";
            g.drawString(info, kPad, kTop + kLine * 3 + 4);
            g.drawString("Shift+键 输入大写和符号", kPad, kTop + kLine * 4 + 4);
            const bool last = form.index + 1 == static_cast<int>(form.fields.size());
            footer(g, String(last ? "Enter 确定" : "Enter 下一项") + (f.secret ? " Fn+S 明文" : "") + " Fn+Aa 大写 `返回");
            break;
        }
        case View::Message:
            text_block(g, message, message_ok ? kAccent : kWarn);
            footer(g, "任意键继续");
            break;
        case View::Keys: {
            std::vector<Line> lines;
            for (int i = 0; i < kProviderCount; ++i) lines.push_back({kProviders[i].label, settings_store::key_hint(provider_key(i))});
            list(g, lines);
            footer(g, "; . 选择  Enter 修改  ` 返回");
            break;
        }
        case View::Hosts: {
            std::vector<Line> lines;
            for (const auto& p : config::get().profiles) lines.push_back({p.id, p.user + "@" + p.host});
            lines.push_back({"新增主机…", "", kAccent});
            lines.push_back({"设备公钥（二维码）", "", kAccent});
            list(g, lines);
            footer(g, "; . 选择  Enter 打开  ` 返回");
            break;
        }
        case View::Host: {
            const SshProfile* p = nullptr;
            for (const auto& item : config::get().profiles) {
                if (item.id == host_id) p = &item;
            }
            if (!p) {
                text_block(g, "这台主机已不存在", kWarn);
            } else {
                g.setTextColor(kText, kBg);
                g.drawString(fit(g, p->user + "@" + p->host + ":" + String(p->port), kWidth - kPad * 2), kPad, kTop + 2);
                g.setTextColor(kMuted, kBg);
                g.drawString(p->key_path.isEmpty() ? (p->password.isEmpty() ? "登录：每次输入密码" : "登录：保存的密码")
                                                   : "登录：设备密钥",
                             kPad, kTop + kLine + 2);
                row(g, kTop + kLine * 3, true, delete_armed ? "再按 Enter 确认删除" : "删除这台主机", "", kDanger);
            }
            footer(g, "Enter 删除  ` 返回");
            break;
        }
        case View::WebToken: {  // full height like the public key: no title bar over the code
            const String token = web_token::token();
            const String ip = net::ip();
            g.fillScreen(kBg);
            const int x = qr_panel(g, ip.isEmpty() ? token : "http://" + ip + "/?token=" + token);
            g.setFont(&fonts::efontCN_10);
            g.setTextColor(kAccent, kBg);
            g.drawString("网页令牌", x, 2);
            g.setTextColor(kText, kBg);
            int y = 16;
            for (const auto& line : wrap_text(g, token, kWidth - x - 2)) {
                g.drawString(line, x, y);
                y += 11;
            }
            g.setTextColor(kMuted, kBg);
            g.drawString(ip.isEmpty() ? "未联网" : "手机扫码登录", x, y + 4);
            if (!ip.isEmpty()) g.drawString(ip, x, y + 15);
            g.drawString("R 换新令牌  ` 返回", x, kHeight - 12);
            break;
        }
        case View::PublicKey: {
            if (public_key.isEmpty()) {
                String error;
                public_key = ssh::device_public_key(error);  // generates the key on first use
                if (public_key.isEmpty()) public_key = "ERROR: " + error;
            }
            if (public_key.startsWith("ERROR")) {
                text_block(g, public_key, kWarn);
            } else {  // full height like the QR overlay: no title bar over the code
                g.fillScreen(kBg);
                const int x = qr_panel(g, public_key);
                g.setFont(&fonts::efontCN_10);
                g.setTextColor(kAccent, kBg);
                g.drawString("设备公钥", x, 2);
                g.setTextColor(kText, kBg);
                int y = 16;
                for (const auto& line : wrap_text(g, public_key, kWidth - x - 2)) {
                    if (y > kHeight - 12) break;
                    g.drawString(line, x, y);
                    y += 11;
                }
            }
            break;
        }
        case View::Lora:
            list(g, lora_lines());
            footer(g, "; . 选择  Enter 修改  ` 返回");
            break;
        case View::Context: {
            static const char* const kNames[] = {"短", "中（默认）", "长"};
            std::vector<Line> lines;
            for (int i = 0; i < 3; ++i) {
                lines.push_back({String(kNames[i]) + "  " + String(kContextKb[i]) + " KB",
                                 config::get().context_kb == kContextKb[i] ? "当前" : ""});
            }
            list(g, lines);
            g.setTextColor(kMuted, kBg);
            g.drawString("保留原文的对话量；越长记得越多，", kPad, kTop + 3 * kLine + 6);
            g.drawString("每轮也越慢、越费 token", kPad, kTop + 4 * kLine + 6);
            footer(g, "; . 选择  Enter 使用  ` 返回");
            break;
        }
        case View::Font: {
            std::vector<Line> lines;
            for (const uint8_t size : kFontSizes) {
                lines.push_back({String(size) + " 号", config::get().terminal_font == size ? "当前" : ""});
            }
            list(g, lines);
            footer(g, "; . 选择  Enter 使用  ` 返回");
            break;
        }
        case View::Portal: {
            // Full height, no title bar: the code is as large as the screen allows.
            portal_clients = portal::clients();
            g.fillScreen(kBg);
            const int x = qr_panel(g, portal_step == 0 ? portal::wifi_qr() : portal::url());
            g.setTextColor(kAccent, kBg);
            g.drawString("手机设置 " + String(portal_step + 1) + "/2", x, 2);
            g.setTextColor(kText, kBg);
            if (portal_step == 0) {
                g.setTextColor(kMuted, kBg);
                g.drawString("热点", x, 20);
                g.setTextColor(kText, kBg);
                g.drawString(portal::ssid(), x, 34);
                g.setTextColor(kMuted, kBg);
                g.drawString("密码", x, 52);
                g.setTextColor(kText, kBg);
                g.setFont(&fonts::efontCN_14);
                const String pw = portal::password();
                g.drawString(pw.substring(0, 4) + " " + pw.substring(4), x, 66);
                g.setFont(&fonts::efontCN_12);
            } else {
                g.setTextColor(portal_clients > 0 ? kAccent : kMuted, kBg);
                g.drawString(portal_clients > 0 ? "手机已连接" : "等待手机连接", x, 20);
                g.setTextColor(kMuted, kBg);
                g.drawString("浏览器打开", x, 38);
                g.setTextColor(kText, kBg);
                g.drawString(WiFi.softAPIP().toString(), x, 52);
                g.setTextColor(kMuted, kBg);
                g.drawString("多数手机会", x, 70);
                g.drawString("自动弹出", x, 84);
            }
            const String notice = snapshot().notice;
            portal_notice = notice;
            g.setTextColor(notice.startsWith("手机已修改") ? kAccent : kMuted, kBg);
            g.drawString(fit(g, notice.startsWith("手机已修改") ? notice.substring(strlen("手机已修改：")) : "已连接 " + String(portal_clients) + " 台",
                             kWidth - x - 2),
                         x, kHeight - kLine * 2 - 2);
            g.setTextColor(kMuted, kBg);
            g.drawString("; . 切换 ` 关闭", x, kHeight - kLine);
            break;
        }
        case View::Storage: {
            const String kind = storage::kind() == "tf" ? String("TF 卡")
                                : storage::degraded() ? String("板载（降级，TF 卡不可用）") : String("板载");
            text_block(g,
                       "存储：" + kind + "\n已用 " + String(storage::used_bytes() / 1048576.0, 1) + " / " +
                           String(storage::total_bytes() / 1048576.0, 1) + " MB\n慢操作（>1 秒）" + storage::slow_report() +
                           "\n关键配置有板载备份，卡坏也能联网",
                       kText);
            footer(g, "` 返回");
            break;
        }
    }
    g.endWrite();
    dirty = false;
    last_draw = millis();
}

// ---------------------------------------------------------------- WiFi

void restore_setup_ap() {  // a failed join leaves station mode: keep the hotspot reachable offline
    if (boot_offline && net::state() != net::WifiState::Connected) net::start_setup_ap();
}

void finish_online(const String& result) {
    show(result + "\n正在重启以完成启动…", true, View::Menu);
    draw_page();
    delay(1500);
    storage::flush();
    ESP.restart();
}

void open_wifi_list() {
    go(View::WifiList);
    scanned = false;
    draw_page();  // "正在搜索…" during the blocking scan
    networks = wifi::scan();
    scanned = true;
    for (std::size_t i = 0; i < networks.size(); ++i) {
        if (networks[i].ssid == net::ssid()) cursor = i;
    }
    dirty = true;  // show the result now, not at the next key
}

void connect(const String& ssid, const String& password, bool from_form) {
    show("正在连接「" + ssid + "」…", true, View::Menu);
    draw_page();
    bool ok = false;
    const String result = wifi::join(ssid, password, ok);
    if (ok && boot_offline) finish_online(result);
    restore_setup_ap();
    // A wrong password goes back to the form with what was typed.
    show(result, ok, ok ? View::Menu : from_form ? View::Form : View::WifiList, ok ? 0 : cursor);
}

void ask_password(const String& ssid) {
    open_form("WiFi", {{"「" + ssid + "」的密码", "", true}}, View::WifiList, cursor,
              [ssid] { connect(ssid, form.fields[0].value, true); });
}

void choose_network() {
    if (cursor == static_cast<int>(networks.size())) {
        open_form("WiFi", {{"网络名", "", false}, {"密码（开放网络留空）", "", true}}, View::WifiList, cursor, [] {
            String ssid = form.fields[0].value;
            ssid.trim();
            if (ssid.isEmpty()) return show("网络名不能为空", false, View::Form);
            connect(ssid, form.fields[1].value, true);
        });
        return;
    }
    const auto n = networks[cursor];
    if (!n.secured) return connect(n.ssid, "", false);
    if (!wifi::saved(n.ssid)) return ask_password(n.ssid);
    show("正在用保存的密码连接「" + n.ssid + "」…", true, View::Menu);
    draw_page();
    const String result = wifi::join_saved(n.ssid);
    if (net::ssid() == n.ssid) {
        if (boot_offline) finish_online(result);
        return show(result, true, View::Menu);
    }
    restore_setup_ap();
    ask_password(n.ssid);  // the saved password no longer works
}

// ---------------------------------------------------------------- other views

void open_selected_host() {
    const auto& profiles = config::get().profiles;
    if (cursor < static_cast<int>(profiles.size())) {
        host_id = profiles[cursor].id;
        return go(View::Host);
    }
    if (cursor == static_cast<int>(profiles.size())) {
        open_form("新增主机", {{"名称（说话时用的，如 开发机）", ""}, {"地址（IP 或域名）", ""}, {"用户名", ""}, {"端口", "22"}},
                  View::Hosts, cursor, [] {
                      const String result = settings_store::add_profile(form.fields[0].value, form.fields[1].value,
                                                                        form.fields[2].value, form.fields[3].value.toInt());
                      show(result, result.startsWith("已添加"), result.startsWith("已添加") ? View::Hosts : View::Form);
                  });
        return;
    }
    go(View::PublicKey);
}

void lora_enter() {
    const auto& l = config::get().lora;
    const int at = cursor;
    switch (cursor) {
        case 0: return show(lora::set("enabled", l.enabled ? "off" : "on"), true, View::Lora, at);
        case 1:
            return open_form("LoRa", {{"频点 MHz（868–923，按当地规定）", l.frequency_mhz > 0 ? String(l.frequency_mhz, 1) : ""}},
                             View::Lora, at, [at] { show(lora::set("frequency", form.fields[0].value), true, View::Lora, at); });
        case 2:
            return open_form("LoRa", {{"发射功率 dBm（-9–22）", String(l.power_dbm)}}, View::Lora, at,
                             [at] { show(lora::set("power", form.fields[0].value), true, View::Lora, at); });
        default: return show(lora::set("antenna", l.antenna_confirmed ? "off" : "installed"), true, View::Lora, at);
    }
}

int item_count() {
    switch (view) {
        case View::Menu: return kMenuCount;
        case View::WifiList: return networks.size() + 1;
        case View::Keys: return kProviderCount;
        case View::Hosts: return config::get().profiles.size() + 2;
        case View::Lora: return 4;
        case View::Font: return 3;
        case View::Context: return 3;
        default: return 1;
    }
}

void back() {
    switch (view) {
        case View::Menu:
            if (!boot_offline) is_open = false;
            return;
        case View::Form: {
            const View to = form.back;
            const int at = form.back_cursor;
            clear_form();
            return go(to, at);
        }
        case View::Portal:
            portal::stop();
            return go(View::Menu, menu_index(View::Portal));
        case View::Host:
        case View::PublicKey: {  // back onto the row that was opened
            const auto& profiles = config::get().profiles;
            int at = profiles.size() + 1;
            for (std::size_t i = 0; view == View::Host && i < profiles.size(); ++i) {
                if (profiles[i].id == host_id) at = i;
            }
            return go(View::Hosts, at);
        }
        default: return go(View::Menu, menu_index(view));
    }
}

void enter() {
    switch (view) {
        case View::Menu: {
            const View to = kMenu[cursor].view;
            if (to == View::WifiList) return open_wifi_list();
            if (to == View::Portal) {
                if (!portal::start()) return show("热点开启失败，请重试", false, View::Menu, menu_index(View::Portal));
                portal_step = 0;
                portal_clients = -1;
                return go(View::Portal);
            }
            if (to == View::Context) {
                const uint8_t kb = config::get().context_kb;
                return go(View::Context, kb == 32 ? 0 : kb == 96 ? 2 : 1);
            }
            if (to == View::Font) {
                const uint8_t f = config::get().terminal_font;
                return go(View::Font, f == 10 ? 0 : f == 14 ? 2 : 1);
            }
            return go(to);
        }
        case View::WifiList: return choose_network();
        case View::Keys: {
            const int i = cursor;
            return open_form("Key", {{String(kProviders[i].label) + " 的 Key", "", true}}, View::Keys, i, [i] {
                const String result = settings_store::set_provider_key(kProviders[i].id, form.fields[0].value);
                clear_form();
                show(result, result.startsWith("已保存"), View::Keys, i);
            });
        }
        case View::Hosts: return open_selected_host();
        case View::Host:
            if (!delete_armed) {
                delete_armed = true;
                return;
            }
            return show(settings_store::remove_profile(host_id), true, View::Hosts);
        case View::Lora: return lora_enter();
        case View::Font:
            settings_store::set_terminal_font(kFontSizes[cursor]);
            return;
        case View::Context:
            config::get().context_kb = kContextKb[cursor];
            config::save_ui_number("context_kb", kContextKb[cursor]);
            dirty = true;
            return;
        default: return;
    }
}

void form_keys(const Keyboard_Class::KeysState& state) {
    Field& f = form.fields[form.index];
    if (state.fn && state.shift) caps = !caps;
    else if (state.fn && has_char(state, 's')) reveal = !reveal;
    else if (state.del) erase_last(f.value);
    else if (state.enter) {
        if (form.index + 1 < static_cast<int>(form.fields.size())) {
            ++form.index;
            reveal = false;
        } else if (form.submit) {
            form.submit();
        }
    } else if (!state.fn) {
        for (const char c : state.word) f.value += (caps && c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    }
}

}  // namespace

void open(const String& why) {
    is_open = true;
    reason = why;
    boot_offline = net::state() != net::WifiState::Connected;
    if (boot_offline && net::state() != net::WifiState::SetupAp) net::start_setup_ap();
    public_key = String();
    if (boot_offline) open_wifi_list();
    else go(View::Menu);
    draw_page();
}

bool active() { return is_open; }

void handle(const Keyboard_Class::KeysState& state) {
    dirty = true;
    if (view == View::Message) {
        go(after_message, after_cursor);
        return;
    }
    if (has_char(state, '`')) {
        back();
        if (!is_open) clear_form();
        return;
    }
    if (view == View::Form) return form_keys(state);
    if (view == View::WifiList && state.fn && has_char(state, 'r')) return open_wifi_list();
    if (view == View::Portal) {
        if (has_char(state, ';') || has_char(state, '.')) portal_step = 1 - portal_step;
        return;
    }
    if (view == View::WebToken) {
        if (has_char(state, 'r')) {
            web_token::regenerate();
            dirty = true;
        }
        return;
    }
    const int count = item_count();
    if (has_char(state, ';')) cursor = (cursor + count - 1) % count;
    else if (has_char(state, '.')) cursor = (cursor + 1) % count;
    else if (state.enter) enter();
}

void draw(bool force) {
    if (!is_open) return;
    if (view == View::Portal) {
        if (!portal::active()) {
            show("10 分钟无操作，热点已关闭", true, View::Menu, menu_index(View::Portal));
        } else if (portal::clients() != portal_clients) {
            if (portal_clients <= 0 && portal::clients() > 0) portal_step = 1;  // phone joined: show the page code
            dirty = true;
        } else if (snapshot().notice != portal_notice) {
            dirty = true;  // "手机已修改：…"
        }
    }
    if (force || dirty || (view == View::Menu && millis() - last_draw > 5000)) draw_page();
}

void describe(cJSON* screen) {
    if (!is_open) return;
    cJSON* st = cJSON_AddObjectToObject(screen, "settings");
    cJSON_AddStringToObject(st, "view", kViewNames[static_cast<int>(view)]);
    cJSON_AddNumberToObject(st, "cursor", cursor);
    cJSON_AddBoolToObject(st, "boot_offline", boot_offline);
    cJSON_AddNumberToObject(st, "networks", networks.size());
    if (view == View::Form) {
        cJSON_AddStringToObject(st, "form", form.title.c_str());
        cJSON_AddStringToObject(st, "field", form.fields[form.index].label.c_str());
        cJSON_AddNumberToObject(st, "field_index", form.index);
        cJSON_AddNumberToObject(st, "input_length", form.fields[form.index].value.length());
    }
    cJSON_AddBoolToObject(st, "caps", caps);
    cJSON_AddStringToObject(st, "message", view == View::Message ? message.c_str() : "");
    cJSON_AddBoolToObject(st, "message_ok", message_ok);
}

}  // namespace vat::ui::settings
