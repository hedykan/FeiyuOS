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
    "timeout_ms": 60000,
    "total_timeout_ms": 300000,
    "max_tokens": 4000
  },
  "ui": {"brightness": 128, "screen_timeout_seconds": 60, "volume": 40},
  "default_ssh_profile": "开发机"
}
```

- 全部字段可省略，缺省值即上例。
- `stt.provider` 可选 `qwen` 或 `openrouter`（OpenRouter 时 `model` 如 `openai/whisper-large-v3`）。
- `agent.url` 接受任何 OpenAI 兼容的 chat completions 端点；Key 取 `providers.deepseek.api_key`。
- `agent.max_tokens` 是模型每轮最多输出多少 token（256–8000，默认 4000）。写程序时整份源码都在这一轮的输出里，700 会把程序截断；4000 够写 4 KB 以内的程序和打补丁。`agent.timeout_ms`、`agent.total_timeout_ms` 随之放宽到 60 秒（最多 65 秒，HTTP 库的限制）和 300 秒；配置里写着 0.7.1 及以前的默认值（25000、60000）时按新默认值处理。
- 三段时间分开算：`stt.timeout_ms` 是一次语音识别请求；`agent.timeout_ms` 是模型每轮；`agent.total_timeout_ms` 是一个任务里**模型请求的累计时间**。执行工具（SSH、高德、联网搜索、本机动作）不计入，各工具有自己的超时；等待用户确认或输入密码也不计入。运行日志 `agent_done` 记录 `model=` 与 `tools=` 两项耗时。
- `default_ssh_profile`：启动远程 Agent 时未指定主机所用的 profile；缺省为第一个 profile。
- `ui.*` 也会被语音设置（亮度、息屏、音量）写回。
- `ui.status_items`（0.7）：Agent 页顶栏右侧显示哪些项、什么顺序，逗号分隔的 id：`time` 时间、`battery` 电量、`signal` Wi-Fi 信号、`ssid` Wi-Fi 名称、`heap` 剩余内存、`ble` 蓝牙；缺省 `time,battery,signal`，空字符串表示都不显示，放不下时从后往前省略。设置页「顶栏」或 `write settings://statusbar` 修改。
- `ui.animation`（0.7）：设置页光标高亮条滑到新行的动画，`2` 开（缺省，120 ms）、`1` 减弱（60 ms）、`0` 关（直接跳）。设置页「动画」或 `write settings://animation` 修改。
- `ui.reasoning`（0.7.5）：显示思考，`1` 开、`0` 关（缺省）。打开后请求模型时带 `reasoning_effort: "low"`，模型的思考显示在记录详细页。设置页「显示思考」或 `write settings://reasoning`（开 / 关）修改。
- `ui.terminal_font`（10 / 12 / 14）与 `ui.context_kb`（32 / 64 / 96）由设置页写入。

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
    "openrouter": {"api_key": "…"},
    "amap": {"api_key": "…"}
  },
  "http": {
    "github": {"value": "…", "host": "api.github.com"}
  }
}
```

- `wifi.ssid/password` 是开机连接的网络；设备上的 Setup 页（`Fn+W`）会改写它。
- `wifi.networks` 供语音“连接家里 WiFi”使用（`session connect device://local/wifi/家里 WiFi`），连接失败会自动回到主网络。
- 启动时缺少语音识别 Key 或 Agent Key 会进入 Setup 页并提示缺什么。
- `http` 是 http 包的接口令牌（请求头里的 `{secret:名字}`），第一次用时在设备上输入，`host` 是它唯一能发往的网站；要换网站就删掉这一项重新输入。

### 需要哪些 Key、去哪里申请

**必需的只有两个：千问 Key 和 DeepSeek Key**（外加 Wi-Fi）。其余都是可选的，不填也能用，只是少了对应的功能。

| Key | 必需 / 可选 | 用来做什么（不填会怎样） | 去哪里申请 |
|:--|:--|:--|:--|
| 语音识别 Key（千问，`qwen`） | **必需**（二选一：千问或 OpenRouter，默认千问） | 按住说话的语音转文字，模型 `qwen3-asr-flash`，北京地域。不填：开机停在设置页，不能用语音 | [阿里云百炼](https://bailian.console.aliyun.com/)（DashScope）：开通模型服务后在「API-KEY」页创建，选**北京**地域。按音频时长计费（约 0.013 元/分钟），新账号有免费额度 |
| Agent Key（DeepSeek，`deepseek`） | **必需** | 大肥鱼理解你的话、决定做什么；联网搜索和读网页也用它（按次计费）。不填：开机停在设置页，什么指令都执行不了 | [DeepSeek 开放平台](https://platform.deepseek.com/)：注册、充值后在「API keys」页创建 |
| 高德 Key（`amap`） | 可选 | 「我在哪」「附近的咖啡店」、天气、路线。不填：只有这些位置类功能不能用 | [高德开放平台](https://console.amap.com/)：应用管理 → 创建新应用 → 添加 Key，「服务平台」选 **Web 服务** |
| OpenRouter Key（`openrouter`） | 可选 | 备用语音识别（`stt.provider` 设为 `openrouter` 时用 Whisper 代替千问）。用千问就不用填 | [OpenRouter](https://openrouter.ai/keys) |
| 接口令牌（`{secret:名字}`） | 可选（用到才填） | 程序或大肥鱼调需要令牌的网络接口（如 GitHub、和风天气）。不填：只有这些接口调不了 | 各网站自己的控制台。不用事先填：第一次调用时设备弹出输入框，令牌只发给录入时的那个网站 |

前三个 Key 在设备的设置页「Key」里填（也可以用手机扫码或网页填）；也可以让大肥鱼打开输入框（「我要填高德 Key」），**不要把 Key 念给它或打进对话里**。

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
| GET | `/api/mem` | 内存诊断：堆总量与碎片、按大小分档的块统计、每个任务的栈大小与最低余量、512 字节以上的已分配块（地址与大小，不含内容），隔一段时间读两次对比可找出常驻的分配 |
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
