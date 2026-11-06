# Qwen3-ASR-Flash 迁移与实测

更新日期：2026-11-03

## 结论

Cardputer 独立运行版的默认 ASR 已从 OpenRouter `openai/whisper-large-v3`
切换为北京区 `qwen3-asr-flash`。Agent 推理仍使用 DeepSeek；这是两个独立阶段，
不应把 DeepSeek 的 5xx 归因于千问转写。

对本项目而言，Qwen3-ASR-Flash 的主要收益是国内链路、中文命令和专有词上下文，
不是最低单价。23 条既有 PCM 命令全部命中关键词，主机侧 P50 为 741 ms；
同一批 OpenRouter Large V3 的 P50 为 2418 ms。

## 同一批 PCM 的结果

| 服务 | 样本 | 关键词通过 | 平均 CER | P50 | P95 |
|---|---:|---:|---:|---:|---:|
| Qwen3-ASR-Flash，带设备术语上下文 | 23 | 23/23 | 4.04% | 741 ms | 816 ms |
| Qwen3-ASR-Flash，不带上下文 | 23 | 22/23 | 3.21% | 712 ms | 1084 ms |
| OpenRouter Whisper Large V3，既有最佳完整轮 | 23 | 22/23 | 7.18% | 2418 ms | 5613 ms |
| OpenRouter Whisper Large V3 Turbo，既有 9 条轮 | 9 | 7/9 | 17.0% | 2090 ms | 2351 ms |

### Qwen-Audio 3.x 补充对照

2026-11-03 使用相同北京端点、Key、23 条 PCM、16 kHz WAV，并为 3.x 模型配置
即时热词和中英语言提示：

| 模型 | 严格关键词 | 语义正确 | 平均 CER | P50 | P95 |
|---|---:|---:|---:|---:|---:|
| `qwen3-asr-flash` | 23/23 | 23/23 | 4.04% | 741 ms | 816 ms |
| `qwen-audio-3.0-asr-flash` | 23/23 | 23/23 | 0.60% | 882 ms | 1480 ms |
| `qwen-audio-3.1-asr-flash` | 22/23 | 23/23 | 2.27% | 860 ms | 2723 ms |

3.1 的严格关键词唯一失败是把“一号灯”规范化成“1号灯”，对下游工具目标语义
完全等价。3.0 的逐字保真度最好；当前 Qwen3 的中位和尾延迟最好。3.1 首轮还出现
一次 TLS EOF，评测器加入最多两次传输级重试后完成全组；这不计入模型文字错误。

报告：

- `logs/qwen-asr-eval-20261103-182337.json`：Qwen-Audio 3.1
- `logs/qwen-asr-eval-20261103-182408.json`：Qwen-Audio 3.0

不带上下文时，`tmux` 被识别成了 `t m x`；加入 Cardputer、SSH、Codex、
Docker、tmux、nginx 等实体词后通过。因此默认保留 ASR system context。
这个上下文只帮助转写，不会注入 Agent 的操作决策上下文。

报告：

- `logs/qwen-asr-eval-20261103-174425.json`：带上下文
- `logs/qwen-asr-eval-20261103-174449.json`：不带上下文
- `logs/pcm-chain-eval-20261103-120050.json`：OpenRouter 23 条对照

## 真机结果

真实 Cardputer 使用自身 Wi-Fi、板载 LittleFS 配置和板载 Key 请求北京千问：

- Qwen HTTP 200；
- `查看这台 Cardputer 的本机状态` 转写正确；
- ASR 阶段 4183 ms；
- 此前同板 OpenRouter Large V3 Turbo 的 ASR 阶段为 10051 ms；
- 本轮板载 Qwen 约快 2.4 倍。

随后五条本机 PCM 的转写为 5/5 正确。完整操作只成功 1/5，失败来自 Agent
阶段：DeepSeek HTTP 503 两次、HTTP 500 一次、空回复一次。迁移 Agent provider
或增加重试/回退是另一项工作。

## 价格

北京区 `qwen3-asr-flash` 按输入音频时长计费：0.00022 元/秒，即
0.0132 元/分钟；输出不计费。官方还列出 36,000 秒（10 小时）免费额度，
有效期以账号开通、模型发布或申请通过三者中较晚者起算 90 天。

OpenRouter 当前目录价格：

- Whisper Large V3：0.000008 美元/秒，即 0.00048 美元/分钟；
- Whisper Large V3 Turbo：0.000003 美元/秒，即 0.00018 美元/分钟。

按 1 美元约 7.2 元只做量级换算，Qwen 每分钟约为 Large V3 的 3.8 倍、
Turbo 的 10.2 倍。10 秒命令的 Qwen 标价约 0.0022 元，1000 次约 2.2 元，
绝对成本仍很低。

价格会变化，发布前应重新核对：

- <https://help.aliyun.com/zh/model-studio/model-pricing>
- <https://openrouter.ai/openai/whisper-large-v3/apps>
- <https://openrouter.ai/openai/whisper-large-v3-turbo/apps>

> 本文的对比数据来自迁移前的主机侧评估脚本，原始报告在当时的 `logs/` 目录（已被 `.gitignore` 排除，不在仓库中）。评估脚本依赖已删除的电脑 Gateway，因此随 Gateway 一并移除；同一批 PCM 保存在 `tests/pcm/`，单机固件上的识别效果由 [E2E](E2E.zh-CN.md) S1 阶段持续验证。

## 当前设备配置

`/vat/config.json`：

```json
{
  "stt": {
    "provider": "qwen",
    "model": "qwen3-asr-flash",
    "url": "https://cn-beijing.maas.qianwenaiapi.com/api/v1/services/aigc/multimodal-generation/generation",
    "language": "zh",
    "enable_itn": true,
    "timeout_ms": 25000
  }
}
```

`/vat/secrets.json` 的 `providers.qwen.api_key` 在设备配置页填写（见 [配置](CONFIGURATION.zh-CN.md)）。日志和测试报告均不输出 Key。

## 接口选择

当前使用 DashScope 同步接口和 Base64 WAV Data URI。它适合“按住说话、松开提交”
的短命令模式，并允许 system message 提供背景和实体词表。若第二版要做边说边出字，
应评估 WebSocket 的 `qwen-audio-3.1-asr-flash-message` 或 streaming 系列，
而不是继续扩大同步 HTTP 请求。
