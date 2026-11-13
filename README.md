# 肥鱼 OS（FeiyuOS）

跑在 M5Stack Cardputer Adv 上的 AI 原生小操作系统：按住说一句话，大肥鱼（DeepSeek）把它翻译成对资源的操作——本机的屏幕、灯、提示音、传感器、TF 卡文件、提醒、红外、蓝牙、GPS、LoRa，高德的位置与路线，联网搜索和任意网络接口，以及通过 SSH 连接的远端主机和 tmux / Codex / Pi / Claude 会话。

想反复做的事，让大肥鱼写成 **Lua 程序**存在卡上，随时再跑，也可以定时、开机或在某个事件发生时自动运行；程序在自己的**程序页**里显示输出、读键盘输入。能力以**扩展包**的形式挂在统一的 URI 命名空间下：内置的二十多个之外，卡上的 Lua 文件也能成为扩展包，还能从**在线目录**一键安装和更新。写入、物理动作、联网写数据等操作要在设备上按 `Y` 确认。

[English](README.en.md) · [更新记录](CHANGELOG.md) · [下载（Release）](https://github.com/hedykan/FeiyuOS/releases) · [文档导航](docs/README.zh-CN.md)

## 它是什么

肥鱼 OS 是跑在 ESP-IDF / FreeRTOS 之上的应用层，内核仍是 FreeRTOS。它做的是操作系统用户空间的那部分：

| 操作系统里的概念 | 在这里 |
|:--|:--|
| 文件系统命名空间 | URI 资源：`device://` `file://` `sensor://` `timer://` `device://local/ir/` `device://local/ble/` `device://local/lora/` `device://local/wifi/` `place://` `https://` `ssh://<主机>` `memory://` `prog://` `ui://`… |
| 驱动 / 系统调用 | 扩展包认领资源，七个通用工具（`discover` `read` `write` `exec` `observe` `input` `session`）调用它们 |
| man 手册 | `discover` |
| 程序 | `prog://`：卡上的 Lua 5.4 程序（`.lua`），由大肥鱼写、设备运行，通过 `vat.*` 调用各项能力 |
| 终端 / 图形界面 | 程序页（文字输入输出）和图形页（画面），像 Linux 的 tty 和 X |
| 包管理 | `pkg://`：管理卡上的程序和扩展包，在线目录安装、更新、退回 |
| 调度与中断 | `trigger://`：时间、开机或事件触发程序；事件总线 |
| 权限 | Policy：本机可逆操作直接执行，写入和物理动作等设备上按 `Y`；程序声明的权限保存时确认一次 |
| Shell | 大语言模型 |

录音、Agent 循环、程序运行、SSH、界面和设置都在设备上；语音识别（通义千问）和大模型（DeepSeek）在云端，由设备直接调用。电脑只用于编译和刷写固件，运行时不需要任何电脑进程。旧名 Voice Agent Terminal 还留在少数地方（热点名 `VAT-XXXX`、`tools/vat_e2e.py`、卡上的 `/vat` 目录）。

## 硬件与约束

| 项 | 基线 |
|:--|:--|
| 设备 | M5Stack Cardputer Adv（ESP32-S3FN8，双核 240 MHz）；可选 Cap LoRa-1262（GPS + LoRa，见 [硬件基线](docs/TECHNICAL_RESEARCH.zh-CN.md#硬件基线)） |
| Flash / PSRAM | 8 MB / 无 |
| 屏幕 | 240×135 |
| 输入 | 56 键键盘、G0 按键（Btn0）、SPM1423 麦克风 |
| 存储 | 建议插 microSD（程序、扩展包、长对话都在卡上）；不可用时使用板载 LittleFS |
| 网络 | 2.4 GHz Wi-Fi |

没有 PSRAM，所以高内存阶段严格串行：录音写入存储 → 流式上传语音识别 → 关闭 TLS → 请求模型 → 关闭 TLS → SSH。录音不进内存，请求体由 PCM 文件边读边做 WAV 封装和 Base64 编码后直接写入 TLS 连接；每个 Lua 程序跑在自己的私有堆里，结束后整块归还。

## 安装

**用发布版**（推荐）：从 [Release](https://github.com/hedykan/FeiyuOS/releases) 下载 `feiyuos-vX-cardputer-adv.bin` 和 `feiyuos-vX-sd.zip`：

```sh
pip install esptool
# 首次安装和升级都是这一条，写 0x0；不会动 TF 卡和板载存储
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x0 feiyuos-vX-cardputer-adv.bin
# 卡上资源（开机动画、吉祥物、空调码库）：解压到 TF 卡根目录
unzip -o feiyuos-vX-sd.zip -d /Volumes/卡名
```

Windows 上端口一般是 `COMx`，macOS 上是 `/dev/cu.usbmodem*`。也可以在 M5Burner 里搜「FeiyuOS」直接烧录。

**自己编译**：`arduino-cli`、板卡包 `m5stack:esp32 3.2.5`、库 `M5Cardputer`、`LibSSH-ESP32 5.7.0`、`RadioLib 7.8.1`。发布版用自建的 ESP-IDF 库（空闲内存多三四十 KB），见 [firmware/README.md](firmware/README.md)。

```sh
make build              # 编译 firmware/standalone_agent
make upload PORT=/dev/ttyACM0
make monitor            # 可选：查看运行事件（同时写入设备 /vat/logs/runtime.jsonl）
```

## 首次配置

上电后没有可用的 Wi-Fi 时，设备自动进入**设置页**，同时开启热点 `VAT-XXXX`。之后随时按 `Fn+W` 打开设置页。

1. **Wi-Fi**：在设置页搜索附近网络，按编号选择，加密网络在键盘上输入密码。设备先连接、成功后才保存，密码输错不会影响原来的网络。
2. **Key**。**必需的只有两个**，可选的不填也能用，只是少了对应功能：

   | Key | 必需 / 可选 | 去哪里申请 |
   |:--|:--|:--|
   | 千问 Key（语音识别） | **必需** | [阿里云百炼](https://bailian.console.aliyun.com/)「API-KEY」页创建，选北京地域 |
   | DeepSeek Key（大肥鱼） | **必需** | [DeepSeek 开放平台](https://platform.deepseek.com/)「API keys」页创建，需充值 |
   | 高德 Key（位置、天气、路线） | 可选 | [高德开放平台](https://console.amap.com/)创建应用，服务平台选「Web 服务」 |
   | 接口令牌（GitHub 等） | 可选 | 不用事先填：第一次调用时设备弹出输入框，只发给那个网站 |

   各 Key 的用途、不填会怎样见 [配置文档](docs/CONFIGURATION.zh-CN.md#需要哪些-key去哪里申请)。
3. **SSH 主机**（可选，用开发机时才要）：设置页「SSH 主机」逐项填名称、地址、用户、端口；「设备公钥」页用二维码显示公钥，添加到主机的 `authorized_keys`（缺私钥时设备自动生成）。

Key 和主机三种方式任选：设备键盘；手机（设置页「手机设置（扫码）」，扫码加入设备热点后自动弹出设置页）；电脑（同一局域网打开 `http://<设备 IP>`，用设置页显示的网页令牌）。字段说明见 [配置](docs/CONFIGURATION.zh-CN.md)。Key 明文保存在设备上，只应在可信局域网中使用，见 [SECURITY](SECURITY.md)。

## 界面

四个页面，`Btn0` 依次切换，左上角显示当前是哪一页；暗色主题。

- **Agent 页 = 对话时间线**：一句话一行（时间、原话、状态），最新的在最下面。选中的一行展开一句结论；正在执行的显示「识别 · 理解 · 执行 · 总结」进度；需要确认时这一行变成橙色并写出命令；需要密码时弹出掩码输入框。`/` 打开详情：原话、结论、每一步做了什么、原始输出。顶栏右侧可选显示时间、电量、Wi-Fi、剩余内存、蓝牙。
- **SSH 页**：Agent 接入的远端终端（`tmux capture-pane`，每 5 秒刷新）。说「连接开发机」会接入或新建 `vat-shell`；说「让 Codex/Pi 在某项目……」会切到对应会话。远端出现密码提示时，输入自动变成掩码模式。
- **程序页**（`Fn+A`）：列出卡上的程序和本地应用，回车运行 / 打开。程序运行时这一页就是它的控制台：`print` 实时显示，程序要输入时底部出现输入行，`` ` `` 中断；跑完可以翻看输出。让大肥鱼运行程序时也会自动切过来。
- **图形页**：大字、二维码、图片、面板、画布都显示在这里，不盖住对话；有新画面时自动切过来，任意键关闭并回到原来的页面。说「画一个旋转的椭圆」「画个电量表」，大肥鱼用与 M5GFX 同名的绘制指令画（旋转、循环动画、每帧 Lua 计算、绑定数据定时刷新）；面板带选项，选择可以触发程序。画布和面板都能存成本地应用。
- **帮助（`Fn+H`）**：三页，分别是按键、设置（当前值 + 示例说法）、能做的事。后两页由各扩展包自动生成。
- **空调码库**（用空调时需要）：IRext 的空调红外码（MIT），在 Release 的 `sd.zip` 里（`/vat/ir/ac.irdb`）。说「把客厅空调开到 26 度制冷，奥克斯的」，第一次会逐套试码并问你空调有没有反应。
- **蓝牙**（可选，设置 → 蓝牙打开后重启）：扫描附近设备、广播信标、连接设备读写数据与订阅通知、当蓝牙键盘往电脑打字。
- **开机动画与吉祥物**（可选）：像素大肥鱼，素材在 `assets/boot/`、`assets/mascot/`（二创，不在 MIT 授权范围内，见其中的 `NOTICE.md`），在 `sd.zip` 里。

## 程序与扩展包

- **程序**：「写个程序，每天早上看天气，下雨就提醒我带伞」——大肥鱼写成 `/vat/things/lua/名字.lua`，开头用注释声明参数和要用到的权限（`-- @uses write ssh://开发机/*`），保存时你确认一次。运行：说「运行 xxx」，或在程序页选中回车；也能绑定触发（`trigger://`）。写法见 [程序与触发](docs/PROGRAMS.zh-CN.md)。
- **卡上的扩展包**：开头声明 `@package` 和 `@target weather://` 的 `.lua` 就是扩展包，启用后大肥鱼和程序都能用 `weather://`。
- **在线目录**：说「看看有什么可以装的」（`read pkg://catalog`）、「安装 发布」——下载后核对大小、SHA-256 和声明的权限，与目录一致才装；有新版本可以更新，也能退回上一版。目录在本仓库的 [packages/](packages/README.md)。
- **网络接口**：`read https://…`（GET）直接执行，`write https://…`（POST 等）要确认；令牌写成 `{secret:名字}`，在设备上输入一次，只发给录入时的网站。
- 内置扩展包的列表和写法见 [扩展包开发手册](docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md)。

## 按键

| 页面 | 按键 | 作用 |
|:--|:--|:--|
| 全局 | `Btn0` | 切换页面：Agent → SSH → 程序 → 图形（有画面时） |
| 全局 | `` ` ``（Esc） | 取消正在进行的任务或程序；关闭详情和帮助；取消密码输入 |
| 全局 | `Fn+A` | 程序页 |
| 列表 | 按住 `;` / `.` | 连续滚动（设置、时间线、帮助、程序页；SSH 页除外） |
| 全局 | `Fn+H` | 帮助（`,` `/` 翻页） |
| 全局 | `Fn+W` | 设置页（Wi-Fi、Key、SSH 主机、顶栏、动画、LoRa、蓝牙、字号、存储、手机设置、网页令牌） |
| 全局 | `Fn+Space`、`Opt+,`/`Opt+.` | 立即息屏、调整息屏时间 |
| Agent | 按住 `Enter` | 录音，松开后交给 Agent；底部显示音量条和秒数 |
| Agent | 按住 `Fn+Enter` | 录一段进草稿（可多段）；草稿非空时 `Enter` 发送，`Del` 删字，`` ` `` 清空 |
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
| 程序 | `;` `.`、`Enter`、`/`、`Del` | 选择（最近用过的在最上面）、运行 / 打开、看上次的输出、删除（再按 `Y` 确认） |
| 程序（运行中） | 键盘输入、`Enter`、`Del`、`` ` `` | 程序要输入时打字、提交、删字；中断程序 |
| 图形 | 任意键 | 关闭画面（面板：`;` `.` 选、`Enter` 确定；程序运行中按键交给程序） |

**密码不要说出来**：连接新 Wi-Fi、首次连接主机、sudo、接口令牌等需要密码的地方会弹出输入框，在键盘上输入即可。输入的内容不经过语音识别和 AI，也不会被记录。

## 目录

```text
.
├── Makefile                # build / upload / monitor / e2e / release
├── firmware/
│   ├── standalone_agent/   # 产品固件（见 firmware/README.md）
│   │   └── src/packages/   # 内置扩展包（新增能力见 docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md）
│   ├── hardware_probe/     # 键盘/屏幕/麦克风诊断
│   └── sd_probe/           # TF 卡诊断
├── packages/               # 在线目录：可安装的 Lua 程序和扩展包（index.json 由 tools/pkg_index.py 生成）
├── docs/                   # 产品设计、架构、运行时、配置、程序、E2E、证据（导航见 docs/README.zh-CN.md）
├── tests/                  # E2E 与实验台用例、测试程序、语音样本（见 tests/README.md）
├── tools/                  # 自动验收、提示词实验台、发布与目录工具（见 tools/README.md）
└── CHANGELOG.md            # 各版本更新
```

自动验收在 SSH 目标主机上运行，只通过 Wi-Fi 和设备通信：`VAT_DEVICE=http://<设备 IP> python3 tools/vat_e2e.py --phases all`（见 [E2E](docs/E2E.zh-CN.md)）。

## 授权

[MIT](LICENSE)。第三方库、模型和服务保留各自的授权和条款。`firmware/standalone_agent/src/third_party/irext/` 的红外解码和 `assets/irdb/` 的空调码来自 [IRext](https://github.com/irext)（MIT）；`firmware/standalone_agent/src/third_party/lua/` 是 [Lua 5.4](https://www.lua.org/)（MIT）。`assets/mascot/` 和 `assets/boot/` 的吉祥物与开机动画是二创，不在 MIT 授权范围内，见 [NOTICE](assets/mascot/NOTICE.md)、[NOTICE](assets/boot/NOTICE.md)。
