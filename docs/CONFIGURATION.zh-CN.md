# 配置

所有配置都在设备存储的 `/vat` 下（TF 卡优先，否则板载 LittleFS。数据只存一处：插卡开机时，板载 `/vat` 是拔卡期间写的，比卡上新，会整体搬到卡上覆盖同名文件，全部复制成功后清空板载副本），通过设备配置页（`http://<设备 IP>`）或 Web API 编辑。修改后配置页会自动调用 `/api/reload`，无需重启。

```text
/vat/
├── config.json              # 非秘密配置
├── secrets.json             # Wi-Fi 与 API Key（明文）
├── ssh/
│   ├── profiles.json        # SSH 主机
│   └── id_ed25519           # 私钥（OpenSSH 格式，ed25519/rsa/ecdsa）
├── context/                 # 上下文窗口、摘要、状态（见扩展方向 · 长上下文）
├── out/                     # 完整工具输出 out#N（TF 卡）
├── archive/                 # 每天一份：每一步操作与回答（TF 卡；总量超过 256 MB 时删除最旧的几天；无卡时为 memory/operations.jsonl）
├── cases/                   # 案卷（TF 卡）
├── memory/
│   ├── notes.txt            # memory://notes
│   ├── clipboard.txt        # device://local/clipboard
│   ├── timeline.jsonl       # 时间线：每句话的结论、步骤、原始输出（详情页读取）
│   └── tui.json             # 当前 tmux 会话，重启后恢复
├── logs/runtime.jsonl       # 阶段耗时、HTTP 状态、可用堆
├── assets/mascot.bin        # 右下角吉祥物（可选；tools/pack_mascot.py 生成后上传；不在 things/ 下，不进板载镜像）
├── cache/ptt.pcm            # 最近一次录音（下一次录音覆盖）
├── cache/jobs/              # 最近 16 个任务的文本记录（/api/job 读取）
└── tests/                   # E2E 上传的测试音频
```

**板载镜像**：配置、密钥、SSH 主机、设备私钥、笔记（最后 4 KB）和 `things/` 下的设备档案，在板载 Flash 的 `/mirror/` 另存一份，任一被镜像的文件修改时同步并递增版本号（卡上 `/vat/.mirror_rev`、Flash 上 `/mirror/rev`）。卡挂载失败时从镜像恢复出 `/vat` 并进入**降级模式**（`/api/status` 的 `degraded: true`，屏幕提示）：能联网、语音、连主机、联网搜索，上下文退回 8 KB，`out#`、归档检索、案卷不可用。卡恢复后版本号大的一侧为准；降级期间的日志只追加、不覆盖卡上数据。

日志、时间线等文件超过 192 KB（TF 卡）或 48 KB（板载 LittleFS）时轮转为 `*.1.jsonl`；上下文窗口靠压缩控制大小，归档按天分文件，不轮转。v0.3 的 `memory/conversations.jsonl` 在第一次启动 v0.4 时迁入窗口后删除。

## config.json

```json
{
  "stt": {
    "provider": "qwen",
    "model": "qwen3-asr-flash",
    "url": "https://cn-beijing.maas.qianwenaiapi.com/api/v1/services/aigc/multimodal-generation/generation",
    "timeout_ms": 25000
  },
  "agent": {
    "url": "https://api.deepseek.com/chat/completions",
    "model": "deepseek-flash",
    "timeout_ms": 25000,
    "total_timeout_ms": 60000
  },
  "ui": {"brightness": 128, "screen_timeout_seconds": 60, "volume": 40},
  "default_ssh_profile": "开发机"
}
```

- 全部字段可省略，缺省值即上例。
- `stt.provider` 可选 `qwen` 或 `openrouter`（OpenRouter 时 `model` 如 `openai/whisper-large-v3`）。
- `agent.url` 接受任何 OpenAI 兼容的 chat completions 端点；Key 取 `providers.deepseek.api_key`。
- 三段时间分开算：`stt.timeout_ms` 是一次语音识别请求；`agent.timeout_ms` 是模型每轮；`agent.total_timeout_ms` 是一个任务里**模型请求的累计时间**。执行工具（SSH、高德、联网搜索、本机动作）不计入，各工具有自己的超时；等待用户确认或输入密码也不计入。运行日志 `agent_done` 记录 `model=` 与 `tools=` 两项耗时。
- `default_ssh_profile`：启动远程 Agent 时未指定主机所用的 profile；缺省为第一个 profile。
- `ui.*` 也会被语音设置（亮度、息屏、音量）写回。

## secrets.json

```json
{
  "wifi": {
    "ssid": "主网络",
    "password": "…",
    "networks": [
      {"name": "家里 WiFi", "ssid": "Home", "password": "…"}
    ]
  },
  "providers": {
    "qwen": {"api_key": "…"},
    "deepseek": {"api_key": "…"},
    "openrouter": {"api_key": "…"}
  }
}
```

- `wifi.ssid/password` 是开机连接的网络；设备上的 Setup 页（`Fn+W`）会改写它。
- `wifi.networks` 供语音“连接家里 WiFi”使用（`session connect wifi://家里 WiFi`），连接失败会自动回到主网络。
- 启动时缺少语音识别 Key 或 Agent Key 会进入 Setup 页并提示缺什么。

## ssh/profiles.json

```json
{
  "profiles": [
    {
      "id": "开发机",
      "host": "192.168.1.20",
      "port": 22,
      "user": "alice",
      "project_root": "/home/alice/projects",
      "auth": {"type": "private_key", "private_key": "/vat/ssh/id_ed25519"}
    }
  ]
}
```

- `id` 就是语音里说的主机名，模型按原文使用（“开发机”不会被翻译成 devbox）。
- `auth.private_key` 与 `auth.password` 任选；两者都给时先试私钥。
- `project_root`：启动 Codex/Pi/Claude 时，相对项目名（如 `voice-agent-terminal`）解析到这里。
- 模型只看到 profile 名称，看不到主机、用户和凭据。

## Web API

配置页之外，同一组接口供脚本和 E2E 使用：

| 方法 | 路径 | 说明 |
|:--|:--|:--|
| GET | `/api/status` | 存储、网络、堆与最大连续块、socket 数、worker 栈余量、重启原因、电量、忙碌状态、当前屏幕内容（空闲堆不足时只返回精简字段） |
| GET/POST | `/api/file?path=` | 读写 `/vat` 下的文件。GET 分块流式返回，任意大小；`tail=N` 从最后 N 字节内的行首开始，`offset=&length=` 读一段。POST 需 `Content-Type: text/plain`，只适合小文件 |
| POST | `/api/upload?path=` | multipart 上传二进制（私钥、测试音频） |
| POST | `/api/delete?path=` · GET `/api/ls?path=` | 删除 / 列目录 |
| POST | `/api/job?kind=text&text=` | 文字指令交给 Agent；`kind=pcm|transcribe|draft&path=` 处理录音；`kind=input&text=&keys=&submit=1`、`kind=observe` 操作终端页 |
| GET | `/api/job?id=` | 任务状态（`done` `awaiting_approval` `awaiting_secret` `cancelled` `error`）、识别文本、回复、全部工具调用与耗时 |
| POST | `/api/action?name=` | `approve` `reject` `approve_all` `cancel` `page_agent` `page_ssh` `toggle` `cursor_up` `cursor_down` `cursor_latest` `open_detail` `close` `help` `help_next` `detail_up` `detail_down` `wake` `clear_history` `mic_test` `record_test` |
| POST | `/api/secret` | 提交正在等待的密钥（`text/plain`），效果等同于设备上的输入框；不记录、不回显 |
| GET | `/api/screenshot` | 当前屏幕的 BMP 截图 |
| POST | `/api/reload` · `/api/reboot` | 重新加载配置 / 重启 |

## 当前安全取舍（已知，后续单独处理）

- Key、Wi-Fi 密码、SSH 私钥明文存放在设备存储。
- HTTPS 不校验证书；SSH 不校验主机指纹。
- 配置页和 Web API 没有认证，同一局域网内可读写 `/vat`（包括密钥）并下发指令。
