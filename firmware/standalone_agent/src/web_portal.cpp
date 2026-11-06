#include "web_portal.h"

#include <cJSON.h>

#include "common.h"
#include "config.h"
#include "ops_lora.h"
#include "ops_portal.h"
#include "ops_ssh.h"
#include "settings_store.h"
#include "storage.h"
#include "ui_kit.h"
#include "ui_state.h"

namespace vat::web_portal {
namespace {

WebServer* web = nullptr;

const char kPage[] PROGMEM = R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>VAT 设置</title><style>
:root{--bg:#0f1216;--card:#1a1f26;--text:#e6eaf0;--muted:#7a8494;--accent:#3fb8af;--danger:#e5534b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 -apple-system,system-ui,sans-serif}
header{padding:16px}h1{font-size:18px;margin:0}small,.muted{color:var(--muted)}
section{background:var(--card);margin:0 12px 12px;border-radius:12px;padding:12px 14px}
h2{font-size:15px;margin:0 0 8px;color:var(--accent)}.row{display:flex;align-items:center;gap:8px;padding:6px 0;border-top:1px solid #262c35}
.row:first-of-type{border-top:0}.grow{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
input,select,textarea{width:100%;padding:10px;border-radius:8px;border:1px solid #2f3742;background:#11161c;color:var(--text);font-size:16px;margin:4px 0}textarea{font-family:inherit;resize:vertical}
button{padding:8px 12px;border:0;border-radius:8px;background:#2a323d;color:var(--text);font-size:14px}
button.primary{background:var(--accent);color:#06221f}button.danger{color:var(--danger)}
#toast{position:fixed;left:12px;right:12px;bottom:16px;padding:12px;border-radius:10px;background:#263238;display:none}
code{word-break:break-all;font-size:12px}form{margin-top:6px}
</style></head><body><header><h1>Voice Agent Terminal 设置</h1><small id="dev">读取中…</small></header>
<section><h2>Key</h2><div id="keys"></div></section>
<section><h2>SSH 主机</h2><div id="hosts"></div>
<form id="addhost"><input name="id" placeholder="名称（说话时用的，如 开发机）" required>
<input name="host" placeholder="地址（IP 或域名）" required autocapitalize="off"><input name="user" placeholder="用户名" required autocapitalize="off">
<input name="port" value="22" inputmode="numeric"><button class="primary">新增主机</button></form>
<p class="muted">新主机用设备密钥登录：把下面的设备公钥加入主机 ~/.ssh/authorized_keys，或在 Agent 里说“给 X 配置免密登录”。</p></section>
<section><h2>设备公钥</h2><code id="pub"></code><p><button id="copy">复制</button></p></section>
<section><h2>LoRa</h2><div id="lora"></div></section>
<section><h2>终端字号</h2><select id="font"><option>10</option><option>12</option><option>14</option></select></section><section><h2>角色设定</h2><textarea id="persona" rows="4"></textarea><p><button id="savep">保存</button></p><p class="muted">拼到提示词末尾的角色口吻，只改语气、不影响操作。留空则无角色。</p></section>
<p class="muted" style="margin:0 16px 80px">WiFi 请在设备上设置（开着热点时切换 WiFi 会断开手机）。设置完点“完成”或直接关闭页面。</p>
<div id="toast"></div><script>
const $=s=>document.querySelector(s);
function toast(t){const e=$('#toast');e.textContent=t;e.style.display='block';clearTimeout(e._t);e._t=setTimeout(()=>e.style.display='none',3500)}
async function post(p,d){const r=await fetch('/m/api/'+p,{method:'POST',body:new URLSearchParams(d)});const j=await r.json();toast(j.result||j.error);load();return j}
function el(h){const d=document.createElement('div');d.innerHTML=h;return d.firstElementChild}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function load(){const s=await (await fetch('/m/api/state')).json();
$('#dev').textContent=s.device;const k=$('#keys');k.innerHTML='';
s.keys.forEach(x=>{const r=el(`<div class="row"><div class="grow">${esc(x.label)}<br><small>${esc(x.hint)}</small></div><button>修改</button></div>`);
r.querySelector('button').onclick=()=>{const v=prompt(x.label+' 的新 Key');if(v)post('key',{provider:x.id,key:v})};k.appendChild(r)});
const h=$('#hosts');h.innerHTML='';s.hosts.forEach(x=>{const r=el(`<div class="row"><div class="grow">${esc(x.id)}<br><small>${esc(x.user)}@${esc(x.host)}:${x.port}</small></div><button class="danger">删除</button></div>`);
r.querySelector('button').onclick=()=>{if(confirm('删除 '+x.id+'？'))post('host_delete',{id:x.id})};h.appendChild(r)});
$('#pub').textContent=s.pubkey;const l=$('#lora');l.innerHTML='';
[['enabled','开关',s.lora.enabled?'已开启':'已关闭'],['frequency','频点 MHz（868–923）',s.lora.frequency||'未设置'],['power','发射功率 dBm（-9–22）',s.lora.power],['antenna','天线',s.lora.antenna?'已确认装好':'未确认']].forEach(([key,label,val])=>{
const r=el(`<div class="row"><div class="grow">${label}<br><small>${esc(val)}</small></div><button>修改</button></div>`);
r.querySelector('button').onclick=()=>{let v;if(key=='enabled')v=s.lora.enabled?'off':'on';else if(key=='antenna')v=s.lora.antenna?'off':'installed';else v=prompt(label,s.lora[key]||'');if(v!=null&&v!=='')post('lora',{key,value:v})};l.appendChild(r)});
if(!s.lora.cap)l.appendChild(el('<small>未检测到 Cap LoRa-1262</small>'));$('#font').value=String(s.font);$('#persona').value=s.persona||''}
$('#addhost').onsubmit=e=>{e.preventDefault();const f=new FormData(e.target);post('host_add',Object.fromEntries(f)).then(j=>{if(j.ok)e.target.reset()})};
$('#font').onchange=e=>post('font',{size:e.target.value});$('#savep').onclick=()=>post('persona',{text:$('#persona').value});
$('#copy').onclick=()=>{const t=$('#pub').textContent;(navigator.clipboard?navigator.clipboard.writeText(t):Promise.reject()).then(()=>toast('已复制'),()=>{const r=document.createRange();r.selectNode($('#pub'));getSelection().removeAllRanges();getSelection().addRange(r);toast('已选中，长按复制')})};
load();</script></body></html>)HTML";

void reply(bool ok, const String& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "result", text.c_str());
    web->send(200, "application/json; charset=utf-8", json_print(root));
    cJSON_Delete(root);
}

void changed_by_phone(const String& what) { ui::set_notice("手机已修改：" + what); }

void state() {
    portal::touch();
    const auto& cfg = config::get();
    cJSON* root = cJSON_CreateObject();
    const String device = (portal::active() ? portal::ssid() + " · " : String()) +
                          (storage::kind() == "tf" ? "TF 卡" : "板载存储") + " · 电量 " +
                          String(M5Cardputer.Power.getBatteryLevel()) + "%";
    cJSON_AddStringToObject(root, "device", device.c_str());
    cJSON* keys = cJSON_AddArrayToObject(root, "keys");
    const char* const ids[] = {"qwen", "deepseek", "openrouter", "amap"};
    const char* const labels[] = {"千问（语音识别）", "DeepSeek（Agent）", "OpenRouter（备用语音）", "高德（位置）"};
    const String* values[] = {&cfg.qwen_key, &cfg.deepseek_key, &cfg.openrouter_key, &cfg.amap_key};
    for (int i = 0; i < 4; ++i) {
        cJSON* k = cJSON_CreateObject();
        cJSON_AddStringToObject(k, "id", ids[i]);
        cJSON_AddStringToObject(k, "label", labels[i]);
        cJSON_AddStringToObject(k, "hint", settings_store::key_hint(*values[i]).c_str());
        cJSON_AddItemToArray(keys, k);
    }
    cJSON* hosts = cJSON_AddArrayToObject(root, "hosts");
    for (const auto& p : cfg.profiles) {
        cJSON* h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "id", p.id.c_str());
        cJSON_AddStringToObject(h, "host", p.host.c_str());
        cJSON_AddStringToObject(h, "user", p.user.c_str());
        cJSON_AddNumberToObject(h, "port", p.port);
        cJSON_AddItemToArray(hosts, h);
    }
    String error;
    const String pub = ssh::device_public_key(error);
    cJSON_AddStringToObject(root, "pubkey", pub.isEmpty() ? ("（" + error + "）").c_str() : pub.c_str());
    cJSON* lora = cJSON_AddObjectToObject(root, "lora");
    cJSON_AddBoolToObject(lora, "cap", lora::cap_present());
    cJSON_AddBoolToObject(lora, "enabled", cfg.lora.enabled);
    if (cfg.lora.frequency_mhz > 0) cJSON_AddNumberToObject(lora, "frequency", cfg.lora.frequency_mhz);
    cJSON_AddNumberToObject(lora, "power", cfg.lora.power_dbm);
    cJSON_AddBoolToObject(lora, "antenna", cfg.lora.antenna_confirmed);
    cJSON_AddNumberToObject(root, "font", cfg.terminal_font);
    cJSON_AddStringToObject(root, "persona", cfg.agent_persona.c_str());
    web->send(200, "application/json; charset=utf-8", json_print(root));
    cJSON_Delete(root);
}

}  // namespace

void register_routes(WebServer& server) {
    web = &server;
    server.on("/m", HTTP_GET, [] {
        portal::touch();
        web->send_P(200, "text/html; charset=utf-8", kPage);
    });
    server.on("/m/api/state", HTTP_GET, state);
    server.on("/m/api/key", HTTP_POST, [] {
        portal::touch();
        const String provider = web->arg("provider");
        if (provider != "qwen" && provider != "deepseek" && provider != "openrouter" && provider != "amap") return reply(false, "未知的 Key");
        const String result = settings_store::set_provider_key(provider, web->arg("key"));
        const bool ok = result.startsWith("已保存");
        if (ok) changed_by_phone(provider + " Key");
        reply(ok, result);
    });
    server.on("/m/api/host_add", HTTP_POST, [] {
        portal::touch();
        const String result = settings_store::add_profile(web->arg("id"), web->arg("host"), web->arg("user"),
                                                          web->arg("port").toInt());
        const bool ok = result.startsWith("已添加");
        if (ok) changed_by_phone("新增主机 " + web->arg("id"));
        reply(ok, result);
    });
    server.on("/m/api/host_delete", HTTP_POST, [] {
        portal::touch();
        const String result = settings_store::remove_profile(web->arg("id"));
        const bool ok = result.startsWith("已删除");
        if (ok) changed_by_phone("删除主机 " + web->arg("id"));
        reply(ok, result);
    });
    server.on("/m/api/lora", HTTP_POST, [] {
        portal::touch();
        const String key = web->arg("key");
        if (key != "enabled" && key != "frequency" && key != "power" && key != "antenna") return reply(false, "未知的 LoRa 设置");
        const String result = lora::set(key, web->arg("value"));
        changed_by_phone("LoRa " + key);
        reply(true, result);
    });
    server.on("/m/api/persona", HTTP_POST, [] {
        portal::touch();
        const String result = config::set_agent_persona(web->arg("text"));
        changed_by_phone("角色设定");
        reply(result.startsWith("已"), result);
    });
    server.on("/m/api/font", HTTP_POST, [] {
        portal::touch();
        const int size = web->arg("size").toInt();
        const String refused = settings_store::set_terminal_font(size);
        if (!refused.isEmpty()) return reply(false, refused);
        changed_by_phone("终端字号 " + String(size));
        reply(true, "终端字号已设为 " + String(size));
    });
}

}  // namespace vat::web_portal
