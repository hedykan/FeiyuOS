#include "stt.h"

#include "common.h"
#include "config.h"
#include "net.h"

namespace vat::stt {
namespace {

// Entity words help the recogniser with mixed Chinese/English commands.  The
// context only affects transcription; it never reaches the operation agent.
constexpr char kQwenContext[] =
    "这是 Cardputer AI 操作终端的语音命令。可能出现这些专有词：Cardputer、SSH、Codex、Claude、Pi、"
    "Docker、tmux、nginx、journalctl、systemctl、hostname、git status、Control C、开发机、"
    "voice-agent-terminal、allow、deny、discover。请只转写实际听到的内容，不要回答或执行命令。";

String json_escape(const char* text) {
    cJSON* value = cJSON_CreateString(text);
    String printed = json_print(value);
    cJSON_Delete(value);
    return printed;  // includes quotes
}

}  // namespace

Transcript transcribe_pcm(const String& pcm_path) {
    const auto& cfg = config::get();
    const unsigned long started = millis();
    Transcript transcript;
    runtime_event("stt_start", cfg.stt_model);

    String prefix;
    String suffix;
    String url;
    String key;
    if (cfg.stt_provider == "qwen") {
        url = cfg.stt_url;
        key = cfg.qwen_key;
        prefix = "{\"model\":" + json_escape(cfg.stt_model.c_str()) +
                 ",\"input\":{\"messages\":[{\"role\":\"system\",\"content\":[{\"text\":" +
                 json_escape(kQwenContext) +
                 "}]},{\"role\":\"user\",\"content\":[{\"audio\":\"data:audio/wav;base64,";
        suffix = "\"}]}]},\"parameters\":{\"asr_options\":{\"language\":\"zh\",\"enable_itn\":true}}}";
    } else {
        url = "https://openrouter.ai/api/v1/audio/transcriptions";
        key = cfg.openrouter_key;
        prefix = "{\"model\":" + json_escape(cfg.stt_model.c_str()) +
                 ",\"language\":\"zh\",\"input_audio\":{\"data\":\"";
        suffix = "\",\"format\":\"wav\"}}";
    }

    const auto response = net::post_pcm_base64(url, key, prefix, pcm_path, suffix, config::get().stt_timeout_ms);
    transcript.elapsed_ms = millis() - started;
    if (!response.ok()) {
        transcript.text = response.status == -2
                              ? String("已取消")
                              : "语音识别失败 HTTP " + String(response.status) + " " +
                                    utf8_head(response.error.isEmpty() ? response.body : response.error, 160);
        runtime_event("stt_error", transcript.text);
        return transcript;
    }

    cJSON* root = cJSON_Parse(response.body.c_str());
    const cJSON* text = root ? cJSON_GetObjectItemCaseSensitive(root, "text") : nullptr;
    if (root && cfg.stt_provider == "qwen") {
        const cJSON* output = json_obj(root, "output");
        const cJSON* choices = output ? cJSON_GetObjectItemCaseSensitive(output, "choices") : nullptr;
        const cJSON* choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
        const cJSON* message = choice ? json_obj(choice, "message") : nullptr;
        const cJSON* content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : nullptr;
        const cJSON* first = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : nullptr;
        text = first ? cJSON_GetObjectItemCaseSensitive(first, "text") : nullptr;
    }
    if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
        transcript.ok = true;
        transcript.text = text->valuestring;
        transcript.text.trim();
    } else {
        transcript.text = "语音识别没有返回文字";
    }
    cJSON_Delete(root);
    runtime_event(transcript.ok ? "stt_done" : "stt_empty",
                  String(transcript.elapsed_ms) + "ms " + utf8_head(transcript.text, 120));
    return transcript;
}

}  // namespace vat::stt
