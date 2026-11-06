# 肥鱼 OS（FeiyuOS）

跑在 M5Stack Cardputer Adv 上的 AI 原生小操作系统：按住说一句话，大肥鱼（DeepSeek）把它翻译成对资源的操作——本机的屏幕、灯、提示音、传感器、TF 卡文件、提醒、红外、GPS、LoRa，高德的位置与路线，联网搜索，以及通过 SSH 连接的远端主机和 tmux / Codex / Pi 会话。几件事可以组合成一段程序存在卡上，随时再跑，也可以定时或开机自动运行。能力以扩展包的形式挂在统一的 URI 命名空间下，写入、物理动作等操作要在设备上按 `Y` 确认（定位与分层见[单机架构](docs/ARCHITECTURE.zh-CN.md)，程序见[程序与触发](docs/PROGRAMS.zh-CN.md)）。

肥鱼 OS 是跑在 ESP-IDF / FreeRTOS 之上的应用层，内核仍是 FreeRTOS。它做的是操作系统用户空间的那部分：统一的资源命名、扩展包（相当于驱动）、权限、程序与调度、用自然语言当 shell。

录音、Agent 循环、程序运行、SSH、界面和设置都在设备上；语音识别（通义千问）和大模型（DeepSeek）在云端，由设备直接调用。电脑只用于编译和刷写固件，运行时不需要任何电脑进程，也不需要 USB 数据线。旧名 Voice Agent Terminal 还留在少数地方（热点名 `VAT-XXXX`、`tools/vat_e2e.py`、卡上的 `/vat` 目录）。

## 硬件与约束

| 项 | 基线 |
|:--|:--|
| 设备 | M5Stack Cardputer Adv（ESP32-S3FN8，双核 240 MHz）+ Cap LoRa-1262（GPS + LoRa，见 [硬件基线](docs/TECHNICAL_RESEARCH.zh-CN.md#硬件基线)） |
| Flash / PSRAM | 8 MB / 无 |
| 屏幕 | 240×135 |
| 输入 | 56 键键盘、G0 按键（Btn0）、SPM1423 麦克风 |
| 存储 | microSD 优先；不可用时使用板载 LittleFS（约 1.5 MB） |
| 网络 | 2.4 GHz Wi-Fi |

没有 PSRAM，所以高内存阶段严格串行：录音写入存储 → 流式上传语音识别 → 关闭 TLS → 请求模型 → 关闭 TLS → SSH。录音不进内存，请求体由 PCM 文件边读边做 WAV 封装和 Base64 编码后直接写入 TLS 连接。

## 构建与刷写

环境：`arduino-cli`、板卡包 `m5stack:esp32 3.2.5`、库 `M5Cardputer`、`LibSSH-ESP32 5.7.0`、`RadioLib 7.8.1`（Cap LoRa-1262）。

```sh
make build              # 编译 firmware/standalone_agent
make upload PORT=/dev/ttyACM0
make monitor            # 可选：查看运行事件（同时写入设备 /vat/logs/runtime.jsonl）
```

刷写只覆盖程序分区，设备上的 `/vat` 配置、私钥和历史不会丢失。

## 首次配置

上电后没有可用的 Wi-Fi 时，设备自动进入**设置页**，同时开启热点 `VAT-XXXX`。之后随时按 `Fn+W` 打开设置页。

1. **WiFi**：在设置页搜索附近网络，按编号选择，加密网络在键盘上输入密码。设备先连接、成功后才保存，密码输错不会影响原来的网络。
2. **Key 与 SSH 主机**，三种方式任选：
   - 在设备上：设置页的“Key”填语音识别 Key（Qwen）和 Agent Key（DeepSeek）；“SSH 主机”逐项填名称、地址、用户、端口；“设备公钥”页用二维码显示公钥，添加到主机的 `authorized_keys` 即可（缺私钥时设备自动生成）。
   - 用手机：设置页选“手机设置（扫码）”，扫第一个二维码加入设备热点（随机 8 位数字密码），手机会自动弹出设置页；改完设备上会提示。
   - 用电脑：同一局域网内打开 `http://<设备 IP>`（或 `http://vat.local`）的配置页，直接编辑 `/vat/secrets.json`、`/vat/config.json`、`/vat/ssh/profiles.json`。

字段说明见 [配置](docs/CONFIGURATION.zh-CN.md)。密钥明文保存在设备上，只应在可信局域网中使用，见 [SECURITY](SECURITY.md)。

## 界面

两个页面，`Btn0` 切换；只有顶栏（页面标签 · 主机 · WiFi 信号），暗色主题。

- **Agent 页 = 对话时间线**：一句话一行（时间、原话、状态），最新的在最下面。选中的一行展开一句结论；正在执行的显示“识别 · 理解 · 执行 · 总结”进度；需要确认时这一行变成橙色并写出命令；需要密码时弹出掩码输入框。`/` 打开详情：原话、结论、每一步做了什么、原始输出。
- **SSH 页**：Agent 接入的远端终端（`tmux capture-pane`，每 5 秒刷新），10 号字体，顶栏右侧显示“主机 · 会话”和在线点。终端由 Agent 打开：说“连接开发机”会接入或新建 `vat-shell`；说“让 Codex/Pi 在某项目……”会切到对应会话。远端出现密码提示时，输入自动变成掩码模式。
- **帮助（`Fn+H`）**：三页，分别是按键、设置（当前值 + 示例说法）、能做的事。后两页由各扩展包的 `hints()` 自动生成。
- **吉祥物**（可选）：Agent 页右下角的像素大肥鱼，随最新任务变化（待机、识别、理解、执行、出错）。素材在 `assets/mascot/`（二创，不在 MIT 授权范围内，出处与侵权联系方式见其中的 `NOTICE.md`），`make mascot-upload DEVICE=http://<设备 IP>` 打包并传到卡上 `/vat/assets/mascot.bin`；卡上没有这个文件就不显示。

## 按键

| 页面 | 按键 | 作用 |
|:--|:--|:--|
| 全局 | `Btn0` | 切换 Agent / SSH |
| 全局 | `` ` ``（Esc） | 取消正在进行的任务；关闭详情和帮助；取消密码输入 |
| 全局 | `Fn+H` | 帮助（`,` `/` 翻页） |
| 全局 | `Fn+Space`、`Opt+,`/`Opt+.` | 立即息屏、调整息屏时间 |
| 全局 | `Fn+W` | 设置页（WiFi、Key、SSH 主机、LoRa、字号、存储、手机设置） |
| Agent | 按住 `Enter` | 录音，松开后交给 Agent；底部显示音量条和秒数 |
| Agent | `;` `.` | 在时间线上上下移动 |
| Agent | `/` 或 `Fn+L` | 打开详情（`;` `.` 滚动，`,` 返回） |
| Agent | `,` | 回到最新一条 |
| Agent | `Y` / `N` / `A` | 执行 / 取消 / 本任务后续都执行（有待确认项时） |
| Agent | `Fn+Tab` | 只转写模式（不执行） |
| Agent | `Fn+Del` | 清空对话记忆 |
| 密码输入 | 键盘输入、`Enter`、`` ` ``、`Fn+S` | 输入、确认、取消、显示/隐藏明文 |
| SSH | 键盘输入 | 编辑草稿；`Enter` 发送草稿并回车，空草稿时发送 Enter |
| SSH | 按住 `Fn+Enter` | 语音生成草稿（不自动发送） |
| SSH | `Tab`、`Ctrl+C`、`` ` ``、`Del` | 发送 Tab、C-c、Esc、退格到终端 |
| SSH | `Fn+;` / `Fn+.` | 发送上 / 下方向键 |
| SSH | `Fn+R` / `Fn+V` | 刷新画面 / 粘贴本机剪贴板到草稿 |

**密码不要说出来**：连接新 WiFi、首次连接主机、sudo 等需要密码的操作会弹出输入框，在键盘上输入即可。输入的内容不经过语音识别和 AI，也不会被记录。

## 目录

```text
.
├── Makefile                # build / upload / monitor / e2e
├── firmware/
│   ├── standalone_agent/   # 产品固件（见 firmware/README.md）
│   │   └── src/packages/   # 扩展包（新增能力见 docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md）
│   ├── hardware_probe/     # 键盘/屏幕/麦克风诊断
│   └── sd_probe/           # TF 卡诊断
├── docs/                   # 产品设计、架构、运行时、配置、E2E、证据、人工验收（导航见 docs/README.zh-CN.md）
├── tests/                  # E2E 与实验台用例、TTS 合成的语音样本（见 tests/README.md）
├── tools/                  # 自动验收、提示词实验台、语音合成、发布快照（见 tools/README.md）
└── CHANGELOG.md            # 各版本更新
```
