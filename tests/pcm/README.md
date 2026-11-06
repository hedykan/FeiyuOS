# PCM 语音用例

合成的回归音频，不是用户录音。

- 格式：PCM16 小端、单声道、16 kHz（原始数据，无 WAV 头）
- `manifest.json`：23 条意图用例（转写关键词 + 期望工具调用）
- `standalone_manifest.json`：5 条本机设置用例
- `generated.json`：生成时的哈希、时长、TTS 模型（OpenRouter `x-ai/grok-voice-tts-1.0`，voice `ara`）

由 `tools/vat_e2e.py` 的 S1 阶段上传到设备 `/vat/tests/`，设备自行完成识别与执行。
