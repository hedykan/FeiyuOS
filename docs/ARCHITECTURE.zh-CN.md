# 单机架构

Cardputer 接上电源、连上 Wi-Fi 就能完整工作。没有电脑端进程，没有串口协议，也没有备用的桥接模式。

## 定位：跑在单片机上的 agent shell

设备的能力不是写死在菜单里的功能，而是像操作系统用户态那样组织起来的：

| 操作系统概念 | 本项目 |
|:--|:--|
| 文件系统命名空间 | URI 资源：`device://` `file://` `sensor://` `timer://` `ir://` `lora://` `wifi://` `ssh://<主机>` `memory://` |
| 驱动 / 系统调用 | 扩展包（`src/packages/`）认领资源；底层适配器 `src/ops_*` |
| man 手册 | `discover`：列出资源、能力与用法 |
| 权限 | Policy：本机可逆操作直接执行，其余挂起等待设备上按 `Y` |
| shell | 大模型：把一句话翻译成七个通用工具对资源的调用 |
| 内核 | ESP-IDF / FreeRTOS（不在本项目范围内） |

```text
按键 / 语音 ─▶ 语音识别 ─▶ Agent 循环（模型 + 七个工具）
                                 │ discover / read / write / exec / session / observe / input
                                 ▼
                 Registry：按 URI 把调用路由到认领它的扩展包
                                 │
                 Policy：允许 / 确认 / 拒绝；需要密钥时弹设备输入框
                                 ▼
          驱动与协议：TF 卡、屏幕、IMU、红外、Grove、GPS、LoRa、WiFi、libssh、HTTPS
```

新能力只以扩展包的方式加入，工具数量和 schema 不变，见[扩展包开发手册](EXTENSION_PACKAGE_GUIDE.zh-CN.md)。

```text
┌──────────────────────────── Cardputer ─────────────────────────────┐
│                                                                     │
│  UI 任务（Arduino loop，core 1）          Worker 任务（24 KB 栈，core 0）│
│  ├─ 键盘 / Btn0 / 录音（PTT → /vat/cache/ptt.pcm）   ├─ STT：PCM→WAV→Base64 流式上传 │
│  ├─ Agent 时间线 / SSH / 帮助 / Setup  ◀── 状态 ──┤  Agent：多步任务（可暂停）   │
│  ├─ Y/N/A、密钥输入、Esc ────────── 任务队列 ──────▶ │  Policy → Registry（扩展包） │
│  └─ Web 配置页 + JSON API（:80, mDNS vat.local）     │  ├─ local / wifi             │
│                                                      │  ├─ ssh（libssh）            │
│  存储 /vat（TF 优先，LittleFS 兜底）                  │  └─ tmux / agents           │
│  config · secrets · ssh · memory · logs              └──────────────┬──────────────┘
└─────────────────────────────────────────────────────────────────────┼───────────────┘
              Wi-Fi                                                    │ SSH
  Qwen3-ASR（北京）    DeepSeek chat completions                       ▼
                                                        远端 Linux / tmux / Codex / Pi
```

## 两个任务

- **UI 任务**只做绘制、键盘、录音、配置页请求，从不发起网络请求，所以模型或 SSH 等待期间仍能切页、滚动、取消。
- **Worker 任务**是唯一执行网络与 SSH 的地方，一次只处理一个任务（语音指令、文字指令、TEXT 转写、终端草稿、终端输入、终端刷新，以及暂停任务的 Y / N / A / 密钥 / 放弃）。libssh 与 mbedTLS 实测最深用到约 16 KB，栈设为 24 KB（1.5 倍余量）。
- 两者通过 `ui_state`（互斥锁保护的界面模型）和一个长度为 2 的任务队列通信；屏幕只由 UI 任务绘制。

## 启动顺序

```text
挂载存储（TF → LittleFS；插卡时把 LittleFS 上的 /vat 搬到卡上，数据只存一处）→ 读取配置 → 恢复对话记忆与历史 → 启动 worker
→ 连接 Wi-Fi（18 s）
     ├─ 失败或没有配置 → 开热点 VAT-XXXX + Web 配置页 + 键盘 Setup 页
     └─ 成功 → libssh 初始化 → Web 配置页 → 缺 Key 时进入 Setup，否则 READY
```

## 内存策略

Cardputer Adv 没有 PSRAM。长时间运行的主要风险不是“内存不够”，而是**碎片化**：mbedTLS 每个连接要两块约 16.7 KB 的连续缓冲区，堆里还有 90 KB 空闲时也可能凑不出来。对应措施：

| 措施 | 作用 |
|:--|:--|
| TLS 预留 | 开机占住两块（约 17.9 KB + 5.1 KB），mbedTLS 通过自定义分配直接使用，不再释放和收回，避免它们在堆里挪位置造成碎片 |
| 请求体流式发送 | 系统提示词与工具 schema 以转义好的 JSON 常量放在 Flash，和历史、本轮消息按片段直接写进 TLS，不拼完整请求体 |
| 语音不落第二份 | PCM 边读边加 WAV 头、边做 Base64，直接写进 TLS |
| 历史与记录放存储 | 对话记忆是 TF 卡上的上下文窗口，长度由 `ui.context_kb`（32 / 64 / 96 KB）决定，每次请求从卡上读；任务记录的文本字段写到 `/vat/cache/jobs/` |
| 阶段串行 | 语音识别、模型、SSH 依次进行；请求模型前先关闭 SSH 会话 |
| Web 低内存保护 | 空闲堆低于 40 KB 时配置页接口返回精简状态或 503，不与正在握手的任务争内存 |
| 历史校验 | 读回和写入对话记忆时检查消息结构，避免一次分配失败的残缺记录污染后续请求 |

存储：TF 卡最长录音 30 s，日志按 192 KB 轮转；只有板载 LittleFS（1.5 MB）时最长 20 s、日志按 48 KB 轮转。

v0.1 实测（150 轮语音 / 文字混合压测 0 失败）：空闲可用堆稳定在约 78 KB（另有 35 KB TLS 预留）。v0.5 全量验收：最低可用堆约 37 KB，最大连续块不低于 31.7 KB；Flash 占用 82%，静态内存 66.5 KB。各版本数据见 [E2E 证据](evidence/README.md)。

## 源码

| 文件 | 职责 |
|:--|:--|
| `standalone_agent.ino` | 启动与主循环 |
| `src/storage.*` | 文件系统选择、原子写、JSONL 追加与轮转、尾部读取 |
| `src/config.*` | 三个配置文件的加载与回写 |
| `src/net.*` | Wi-Fi/热点、HTTPS（可取消、重试）、流式请求体（内存片段 + 文件片段、Base64 音频）、响应直接写文件 |
| `src/stt.*` | Qwen / OpenRouter 语音识别 |
| `src/agent.*`、`src/agent_prompt.h` | 工具循环、请求组装（流式）、审批、任务后压缩 |
| `src/context.*` | 上下文窗口与摘要、完整输出 out#N、归档与检索、案卷 |
| `src/policy.*` | 权限决策 |
| `src/registry.*` | 包接口、注册表、discover、按 `supports()` 路由 |
| `src/packages/*` | 扩展包：`memory` `local` `wifi` `web` `ssh` `tmux` `agents` `policy` `catalog`（命令目录）`display` `hardware` `files` `timer` `grove` `ir` `gps` `lora`；`builtin.cpp` 是显式注册清单 |
| `src/ops_*` | 底层适配器（由包调用）：`local` 本机状态与设置、`ssh`、`tui`（tmux）、`web`（含流式 JSON 扫描）、`wifi`（扫描与先连后存）、`hw`（RGB 灯、提示音、IMU、充电推断）、`timer`、`ir`、`gps`、`lora`、`portal`（手机设置热点与 DNS） |
| `src/worker.*` | 任务队列与任务记录 |
| `src/ui.*`、`src/ui_state.*` | 时间线与详情、SSH 页、帮助、密钥输入、录音与音量条、全屏浮层；时间线数据与持久化 |
| `src/ui_kit.*` | 主题色、尺寸、终端字号、文字换行等绘制工具 |
| `src/settings_page.*`、`src/settings_store.*` | 设备设置页（WiFi、Key、SSH 主机、LoRa、字号、存储、手机设置）与配置写入 |
| `src/web.*`、`src/web_portal.*` | 配置页与 JSON API；手机设置页 `/m`（热点来的客户端不能访问 `/api/*`） |
| `src/common.*` | UTF-8 截断、JSON 辅助、shell 转义、取消令牌、运行日志 |

运行时细节见 [操作 Agent 运行时](AGENT_RUNTIME.zh-CN.md)，配置见 [配置](CONFIGURATION.zh-CN.md)。
