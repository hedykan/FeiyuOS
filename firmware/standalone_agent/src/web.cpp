#include "web_token_core.h"
#include "web_token.h"
#include "arena.h"
#include "web.h"

#include <ESPmDNS.h>
#include <functional>
#include <WebServer.h>
#include <M5Cardputer.h>

#include "agent.h"
#include "common.h"
#include "config.h"
#include "context.h"
#include "diag_mem.h"
#include "ops_local.h"
#include "ops_portal.h"
#include "storage.h"
#include "ui.h"
#include "web_portal.h"
#include "worker.h"

namespace vat::web {
namespace {

WebServer server(80);
File upload_file;
String upload_error;

constexpr char kPage[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>VAT 配置</title>
<style>body{font-family:system-ui,sans-serif;margin:16px;max-width:820px}textarea{width:100%;height:180px;font-family:monospace}
button{margin:4px 4px 12px 0}pre{background:#f4f4f4;padding:8px;white-space:pre-wrap}</style></head><body>
<h2>Voice Agent Terminal</h2><pre id="status">loading…</pre>
<label>文件 <select id="path"><option>/vat/config.json</option><option>/vat/secrets.json</option>
<option>/vat/ssh/profiles.json</option></select></label>
<button onclick="load()">读取</button><button onclick="save()">保存</button>
<textarea id="text"></textarea>
<p>上传私钥/文件：<input id="dest" value="/vat/ssh/id_ed25519"> <input type="file" id="file"> <button onclick="up()">上传</button></p>
<p>文字指令：<input id="cmd" size="40"> <button onclick="run()">执行</button></p><pre id="out"></pre>
<script>
const $=id=>document.getElementById(id);
async function status(){$('status').textContent=JSON.stringify(await (await fetch('/api/status')).json(),null,1)}
async function load(){$('text').value=await (await fetch('/api/file?path='+encodeURIComponent($('path').value))).text()}
async function save(){const r=await fetch('/api/file?path='+encodeURIComponent($('path').value),{method:'POST',body:$('text').value});
$('out').textContent=await r.text();await fetch('/api/reload',{method:'POST'});status()}
async function up(){const f=new FormData();f.append('file',$('file').files[0]);
const r=await fetch('/api/upload?path='+encodeURIComponent($('dest').value),{method:'POST',body:f});$('out').textContent=await r.text()}
async function run(){const r=await (await fetch('/api/job?kind=text&text='+encodeURIComponent($('cmd').value),{method:'POST'})).json();
if(!r.id){$('out').textContent=JSON.stringify(r);return}
for(;;){await new Promise(x=>setTimeout(x,1000));const j=await (await fetch('/api/job?id='+r.id)).json();
$('out').textContent=JSON.stringify(j,null,1);if(!['queued','running'].includes(j.status))break}status()}
status();load();
</script></body></html>)HTML";

// Requests are served on the UI task while the worker may be in the middle of
// a TLS handshake; heavy responses wait instead of competing for the heap.
constexpr uint32_t kLowHeap = 40000;
bool low_memory() { return ESP.getFreeHeap() < kLowHeap; }

bool valid_path(const String& path) {
    return path.startsWith("/vat/") && path.indexOf("..") < 0;
}

void send_json(int code, const String& json) { server.send(code, "application/json; charset=utf-8", json); }

void send_error(int code, const String& message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message.c_str());
    send_json(code, json_print(root));
    cJSON_Delete(root);
}

void handle_status() {
    if (low_memory()) {
        send_json(200, "{\"busy\":" + String(worker::busy() ? "true" : "false") +
                           ",\"low_memory\":true,\"free_heap\":" + String(ESP.getFreeHeap()) + "}");
        return;
    }
    cJSON* root = cJSON_Parse(local::status_json().c_str());
    if (!root) return send_error(503, "status unavailable (low memory), retry");  // was answered with null
    cJSON_AddBoolToObject(root, "busy", worker::busy());
    cJSON_AddBoolToObject(root, "config_ready", config::providers_ready());
    cJSON_AddStringToObject(root, "profiles", config::profile_names().c_str());
    cJSON_AddNumberToObject(root, "history_messages", agent::history_messages());
    cJSON_AddItemToObject(root, "screen", cJSON_Parse(ui::screen_text().c_str()));
    cJSON_AddItemToObject(root, "arena", cJSON_Parse(arena::status_json().c_str()));
    const String body = json_print(root);
    if (body.isEmpty()) {  // out of memory while printing
        cJSON_Delete(root);
        return send_error(503, "status unavailable (low memory), retry");
    }
    send_json(200, body);
    cJSON_Delete(root);
}

void handle_file_get() {
    if (low_memory()) return send_error(503, "low memory, retry");
    const String path = server.arg("path");
    if (!valid_path(path)) return send_error(400, "path must be under /vat/");
    storage::flush();  // logs are written behind
    // Streamed in 1 KB blocks: files (context window, outputs, logs) can be
    // larger than any String the heap could hold.  ?tail=N starts on a line
    // boundary within the last N bytes; ?offset=&length= reads a byte range.
    File file = storage::exists(path) ? storage::fs().open(path, FILE_READ) : File();
    if (!file || file.isDirectory()) return send_error(404, "not found");
    const std::size_t size = file.size();
    const long tail = server.arg("tail").toInt();
    std::size_t start = tail > 0 ? (size > static_cast<std::size_t>(tail) ? size - tail : 0)
                                 : std::min<std::size_t>(server.arg("offset").toInt(), size);
    file.seek(start);
    if (tail > 0 && start > 0) {
        int c;
        while ((c = file.read()) >= 0 && c != '\n') ++start;
        ++start;
    }
    std::size_t length = size > start ? size - start : 0;
    if (server.hasArg("length")) length = std::min<std::size_t>(length, server.arg("length").toInt());
    server.setContentLength(length);
    server.send(200, "text/plain; charset=utf-8", "");
    char buffer[1024];
    while (length > 0) {
        const int n = file.read(reinterpret_cast<uint8_t*>(buffer), std::min(length, sizeof(buffer)));
        if (n <= 0) break;
        server.sendContent(buffer, n);
        length -= n;
    }
    file.close();
}

void handle_file_post() {
    const String path = server.arg("path");
    if (!valid_path(path)) return send_error(400, "path must be under /vat/");
    if (!server.hasArg("plain")) return send_error(400, "send the file as Content-Type: text/plain");
    if (!storage::write_text(path, server.arg("plain"))) return send_error(500, "write failed");
    send_json(200, "{\"written\":" + String(server.arg("plain").length()) + "}");
}

void handle_upload_data() {
    HTTPUpload& upload = server.upload();
    const String path = server.arg("path");
    if (upload.status == UPLOAD_FILE_START) {
        upload_error = "";
        if (!valid_path(path) || !storage::ensure_parent(path)) {
            upload_error = "invalid path";
            return;
        }
        storage::remove(path);
        upload_file = storage::fs().open(path, FILE_WRITE);
        if (!upload_file) upload_error = "open failed";
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (upload_file && upload_file.write(upload.buf, upload.currentSize) != upload.currentSize) {
            upload_error = "storage full";
        }
    } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        if (upload_file) upload_file.close();
        if (upload.status == UPLOAD_FILE_ABORTED) upload_error = "aborted";
    }
}

void handle_upload_done() {
    if (!upload_error.isEmpty()) return send_error(500, upload_error);
    storage::changed(server.arg("path"));  // e.g. the device key: keep the flash mirror current
    send_json(200, "{\"uploaded\":\"" + server.arg("path") + "\"}");
}

void handle_delete() {
    const String path = server.arg("path");
    if (!valid_path(path)) return send_error(400, "path must be under /vat/");
    const bool deleted = storage::remove_tree(path);
    storage::changed(path);
    send_json(200, deleted ? "{\"deleted\":true}" : "{\"deleted\":false}");
}

void handle_ls() {
    if (low_memory()) return send_error(503, "low memory, retry");
    const String path = server.arg("path");
    if (path != "/vat" && !valid_path(path)) return send_error(400, "path must be under /vat");
    send_json(200, storage::list_dir(path));
}

void handle_job_post() {
    const String kind = server.arg("kind");
    worker::Job job;
    if (kind == "text") {
        job.kind = worker::Kind::AgentText;
        job.text = server.arg("text");
        if (job.text.isEmpty()) return send_error(400, "text required");
    } else if (kind == "pcm" || kind == "transcribe" || kind == "draft") {
        job.kind = kind == "pcm" ? worker::Kind::AgentPcm
                   : kind == "transcribe" ? worker::Kind::TranscribePcm
                                          : worker::Kind::DraftPcm;
        job.path = server.arg("path");
        if (!valid_path(job.path) || !storage::exists(job.path)) return send_error(400, "pcm path missing");
    } else if (kind == "input") {
        job.kind = worker::Kind::TerminalInput;
        job.text = server.arg("text");
        job.submit = server.arg("submit") == "1";
        String keys = server.arg("keys");
        while (!keys.isEmpty()) {
            const int comma = keys.indexOf(',');
            job.keys.push_back(comma >= 0 ? keys.substring(0, comma) : keys);
            keys = comma >= 0 ? keys.substring(comma + 1) : String();
        }
    } else if (kind == "observe") {
        job.kind = worker::Kind::TerminalObserve;
    } else if (kind == "call") {  // one tool call Policy allows outright: {"tool": ..., "arguments": {...}}
        job.kind = worker::Kind::Call;
        job.text = server.arg("call");
        if (job.text.isEmpty()) return send_error(400, "call required");
    } else if (kind == "discover") {  // prompt lab: the exact discover answer the model would get
        job.kind = worker::Kind::Discover;
        job.text = server.arg("args");
        if (job.text.isEmpty()) return send_error(400, "args required");
    } else {
        return send_error(400, "unknown kind");
    }
    const uint32_t id = worker::submit(job);
    if (!id) return send_error(409, "busy");
    send_json(200, "{\"id\":" + String(id) + "}");
}

void handle_job_get() {
    if (low_memory()) return send_error(503, "low memory, retry"); send_json(200, worker::record_json(server.arg("id").toInt())); }

void handle_action() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "result", ui::action(server.arg("name")).c_str());
    send_json(200, json_print(root));
    cJSON_Delete(root);
}

// Same path as the device's masked input: the value goes straight to the
// paused step and is never logged, echoed or stored in a job record.
void handle_secret() {
    if (!ui::has_pending()) return send_error(409, "no secret requested");
    worker::Job job;
    job.kind = worker::Kind::Secret;
    job.text = server.arg("plain");
    const uint32_t id = worker::submit(job);
    job.text = String();
    if (!id) return send_error(409, "busy");
    send_json(200, "{\"id\":" + String(id) + "}");
}

// Current screen as a 24-bit BMP, read back from the display row by row.
void handle_screenshot() {
    auto& d = M5Cardputer.Display;
    const int w = d.width();
    const int h = d.height();
    const uint32_t row_bytes = (w * 3 + 3) & ~3u;
    const uint32_t size = 54 + row_bytes * h;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = v >> (8 * i); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    header[26] = 1; header[28] = 24; put32(34, row_bytes * h);
    server.setContentLength(size);
    server.send(200, "image/bmp", "");
    server.sendContent(reinterpret_cast<const char*>(header), sizeof(header));
    std::vector<lgfx::rgb888_t> line(w);
    std::vector<uint8_t> out(row_bytes, 0);
    for (int y = h - 1; y >= 0; --y) {  // BMP rows are bottom-up
        d.readRect(0, y, w, 1, line.data());
        for (int x = 0; x < w; ++x) {
            out[x * 3] = line[x].b;
            out[x * 3 + 1] = line[x].g;
            out[x * 3 + 2] = line[x].r;
        }
        server.sendContent(reinterpret_cast<const char*>(out.data()), row_bytes);
    }
}

volatile bool bench_running = false;  // DIAG
uint32_t bench_hz = 0;
String bench_result;

void handle_reload() {
    const String problem = config::load();
    if (!worker::busy()) context::begin();  // re-read context files edited through the file API
    if (!worker::busy()) ui::show_ready();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "problem", problem.c_str());
    cJSON_AddStringToObject(root, "profiles", config::profile_names().c_str());
    send_json(200, json_print(root));
    cJSON_Delete(root);
}

// The phone-settings hotspot reaches /m only: no files, keys, screenshots or
// key injection for whoever joins it.
bool from_hotspot() { return portal::from_portal(server.client().localIP()); }

bool authorized() {
    return web_token::accepts(web_token::presented(server.header("Authorization"), server.header("Cookie"), server.arg("token")));
}

void api_on(const char* path, HTTPMethod method, std::function<void()> handler) {
    server.on(path, method, [handler] {
        if (from_hotspot()) return send_error(403, "phone settings: use /m");
        handler();
    });
}

void api_on(const char* path, HTTPMethod method, std::function<void()> done, std::function<void()> upload) {
    server.on(
        path, method,
        [done] {
            if (from_hotspot()) return send_error(403, "phone settings: use /m");
            done();
        },
        [upload] {
            if (!from_hotspot()) upload();
        });
}

void to_phone_page() {
    server.sendHeader("Location", portal::url(), true);
    server.send(302, "text/plain", "");
}

}  // namespace

constexpr char kLoginPage[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>FeiyuOS</title><body style='font-family:sans-serif;padding:2em'>"
    "<h3>需要网页令牌</h3><p>在设备上按 Fn+W，进入「网页令牌」，用手机扫码登录；"
    "或在地址后加上 ?token=令牌。</p>";

void begin() {
    static const char* kHeaders[] = {"Authorization", "Cookie"};
    server.collectHeaders(kHeaders, 2);
    // Every /api/* call needs the token; so does the phone page unless it
    // comes through the setup hotspot (joining it means being at the device).
    server.addMiddleware([](WebServer& s, Middleware::Callback next) {
        const String uri = s.uri();
        const bool api = uri.startsWith("/api/");
        const bool phone = (uri == "/m" || uri.startsWith("/m/")) && !from_hotspot();
        if ((api || phone) && !authorized()) {
            send_error(401, "token required (device: Fn+W, web token)");
            return true;
        }
        return next();
    });
    server.on("/", HTTP_GET, [] {
        if (from_hotspot()) return to_phone_page();
        const String arg = server.arg("token");
        if (!arg.isEmpty() && web_token::accepts(arg)) {  // the login QR: keep it as a cookie
            server.sendHeader("Set-Cookie", "vat_token=" + arg + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000");
            server.sendHeader("Location", "/", true);
            return server.send(302, "text/plain", "");
        }
        if (!authorized()) return server.send(401, "text/html; charset=utf-8", kLoginPage);
        server.send(200, "text/html; charset=utf-8", kPage);
    });
    web_portal::register_routes(server);
    api_on("/api/status", HTTP_GET, handle_status);
    // DIAG: heap blocks and task stacks; compare two readings to find what stays allocated.
    api_on("/api/mem", HTTP_GET, [] {
        const String body = diag::mem_json();
        if (body.isEmpty()) return send_error(503, "low memory, retry");
        send_json(200, body);
    });
    api_on("/api/file", HTTP_GET, handle_file_get);
    api_on("/api/file", HTTP_POST, handle_file_post);
    api_on("/api/upload", HTTP_POST, handle_upload_done, handle_upload_data);
    api_on("/api/delete", HTTP_POST, handle_delete);
    api_on("/api/ls", HTTP_GET, handle_ls);
    api_on("/api/job", HTTP_POST, handle_job_post);
    api_on("/api/job", HTTP_GET, handle_job_get);
    api_on("/api/action", HTTP_POST, handle_action);
    api_on("/api/key", HTTP_POST, [] { send_json(200, "{\"result\":\"" + ui::press(server.arg("k")) + "\"}"); });
    api_on("/api/reload", HTTP_POST, handle_reload);
    // DIAG: the benchmark runs in its own task (core 0, like the worker) so
    // the web server stays free to generate load while it runs.
    api_on("/api/sd_bench", HTTP_POST, [] {
        if (worker::busy() || bench_running) return send_error(409, "busy");
        const uint32_t mhz = server.arg("mhz").toInt();
        bench_hz = (mhz ? mhz : 25) * 1000000UL;
        bench_result = "";
        bench_running = true;
        xTaskCreatePinnedToCore(
            [](void*) {
                bench_result = storage::bench(bench_hz);
                bench_running = false;
                vTaskDelete(nullptr);
            },
            "sd-bench", 8192, nullptr, 1, nullptr, 0);
        send_json(200, "{\"started\":true}");
    });
    api_on("/api/sd_bench", HTTP_GET, [] {
        send_json(200, bench_running ? String("{\"running\":true}") : bench_result);
    });
    api_on("/api/secret", HTTP_POST, handle_secret);
    api_on("/api/screenshot", HTTP_GET, handle_screenshot);
    api_on("/api/reboot", HTTP_POST, [] {
        send_json(200, "{\"rebooting\":true}");
        delay(300);
        storage::flush();
        ESP.restart();
    });
    // Captive portal: phone connectivity probes (generate_204,
    // hotspot-detect.html, ...) get a redirect, so the phone opens the page.
    server.onNotFound([] {
        if (from_hotspot()) return to_phone_page();
        send_error(404, "not found");
    });
    server.begin();
    MDNS.begin("vat");
    MDNS.addService("http", "tcp", 80);
}

void loop() { server.handleClient(); }

}  // namespace vat::web
