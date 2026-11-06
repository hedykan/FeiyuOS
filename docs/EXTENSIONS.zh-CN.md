# 扩展方向

本文记录可以按 [扩展包开发手册](EXTENSION_PACKAGE_GUIDE.zh-CN.md) 接入的后续能力。所有扩展都继续使用七个工具，通过新的 target 资源地址区分，不增加第八个工具。

```text
device://room/light-1/power     sensor://desk/temperature     gpio://esp-kitchen/2
ir://living-room/tv             serial://port-1                capture://session-1
ui://panel/main                 security://lab/network
```

## 长上下文（TF 卡）✅ v0.4

卡上存全量，内存只放正在发送的一小块；模型看到的是“开头 + 编号”，需要细节时自己去卡里取。仍是七个工具，能力在 `memory` 包里。实现在 `src/context.*`，运行方式见 [Agent 运行时 · 模型上下文](AGENT_RUNTIME.zh-CN.md#模型上下文由什么组成)。

| 机制 | 做法 |
|:--|:--|
| 上下文窗口 | 每轮对话以 API 原生消息片段追加到 `context/window.jsonl`，请求时作为“文件片段”从卡上流式发送，不进内存。预算 48 KB（无卡 8 KB） |
| 压缩 | 任务结束、回复已上屏后，窗口超预算时把较旧部分（保留最新的半个预算）连同旧摘要交给模型合并成新摘要（≤1200 字，保留主机、路径、数字、发现、out#），再从窗口删掉。窗口只追加，DeepSeek 前缀缓存可命中 |
| 完整输出 | 工具结果超过 4 KB 时整份存成 `out#N`，对话里留前 2 KB 和引用（4 KB 以内完整放在对话里：曾把 1.3 KB 的目录列表存档，模型反而去翻页、重跑）。SSH 输出超过 4 KB 时远端保留临时文件，设备每次取 12 KB（SSH 包始终很小），最多 256 KB；超出时文件末尾写明 |
| 按需取回 | `read memory://out/N?page=P`（每页 4 KB）、`?grep=词`（匹配行带行号）；`read memory://out` 列出最近的输出 |
| 归档与检索 | 每一步操作和每次回答按天追加到 `archive/<日期>.jsonl`；`read memory://history?q=词 词`（空格分隔的词都要出现）返回编号清单 |
| 案卷 | `session(attach, memory://cases/名称)` 设为当前案卷，其要点和最近发现每次自动带入对话；`write memory://cases/名称 mode=append` 记录发现（免确认，与笔记相同），被案卷引用的 out# 不会被清理 |
| 清理 | `out/` 超过 2 GB 从最旧的删（案卷引用的保留）；`cache/` 开机清理 |
| 没有卡 | 窗口 8 KB、结果截到 600 字节（同 v0.3）；`memory` 包不出现在帮助页和 discover 中 |

卡上的目录（`/vat` 下）：

```text
/vat/
├── context/window.jsonl   当前窗口：一行一轮，已转义的消息片段，发送时原样流出
├── context/summary.json   窗口之前的摘要（一条 system 消息片段）
├── context/state.json     下一个输出编号、最旧编号、输出总量、当前案卷
├── out/<编号/1000>/<编号>.txt + .meta.json   完整输出与说明（命令、字节数、时间）
├── archive/<YYYYMMDD>.jsonl                   全量归档
└── cases/<名称>/notes.md findings.jsonl refs.txt   案卷
```

实测（E2E S8）：312 KB 日志问“第 4 个 ERROR”，模型用 grep 从 out# 中找到正确答案，未重新执行命令；取回 256 KB 用 2.4 s；清空对话后从归档答出；案卷跨重启带入；40 KB 窗口下单轮中位数 4.1 s（空窗口 3.9 s）；压缩后旧事实仍可回答；全程最低可用堆 ≥ 53 KB。

## 联网搜索 ✅ v0.4

`web` 包，走 DeepSeek Search / Fetch API（与对话同一个 Key，每 1000 次成功请求 0.75 美元，空结果不计费）：

- `read web://search?q=搜索词[&count=N]`：编号结果（标题、网站、日期、约 600 字摘要、网址）；搜索词 ≤128 字，支持 `site:` `-site:` `"短语"` `lang:` `after:` `before:`；
- `read web://page?url=网址`：网页正文（Markdown），整页存成 `out#N`，对话里是开头 4 KB。

响应直接写到卡上，再用流式 JSON 扫描取字段，网页再大也不进内存。实测：搜索约 5 s，读网页约 15 s。
## 基础扩展包（本机能力补全与外接硬件）✅ v0.5

v0.5 的目标：把 Cardputer Adv 本身和已装的 Cap LoRa-1262 都做成扩展包，后面的即时 UI 和本地应用才有东西可以组合。硬件细节见[技术调研 · 硬件基线](TECHNICAL_RESEARCH.zh-CN.md#硬件基线)。每个包照[扩展包开发手册](EXTENSION_PACKAGE_GUIDE.zh-CN.md)做：`hints()` 进帮助页，清单编号，写操作需要确认，E2E 补用例；没检测到硬件时 `available()` 为 false，帮助页不显示，discover 不披露。

本机（按实际完成情况；最初设计中的 `display://` 写法、空调遥控未采用）：

| 能力 | 状态 | target |
|:--|:--|:--|
| Wi-Fi 扫描、状态、切换、设置新网络 | ✅ `wifi` 包 | `wifi://scan` `wifi://status` `wifi://SSID` |
| 亮度、息屏、音量、剪贴板、电量、笔记 | ✅ `local` 包 | `device://local/*` `memory://*` |
| 屏幕：大字、二维码、卡上的小图片、通知横幅 | ✅ `display` 包：全屏浮层盖在当前页上，任意键返回；大字用已链接字体放大（另加中文字库会超出程序分区）；图片限 32 KB | `device://local/display/{text,qr,image,banner,clear}` |
| 充电状态 | ✅ 并入 `/api/status` 与本机状态：Adv 只有电池 ADC，按 USB 主机在线 + 电压 5 分钟走势推断，注明“推断”；只接充电头时无法判断 | `device://local/status` |
| RGB 指示灯（G21 WS2812） | ✅ `hardware` 包：颜色名、`#RRGGBB` 或 off，免确认 | `device://local/led` |
| IMU（BMI270） | ✅ `hardware` 包：姿态、俯仰 / 横滚、是否在动 | `sensor://imu/...` |
| 提示音 | ✅ `hardware` 包：1–5 声，免确认；扬声器只在两次录音之间临时开启，录音中不响 | `device://local/audio/beep` |
| 红外发送 | ✅ `ir` 包（IRremote 4.5.0，G44）：只做电视（三星 / LG / 索尼 / 飞利浦的电源、音量、静音）；发码后由用户确认是否生效，没反应换下一组。空调需要整套状态码，IRremote 不支持，未做 | `ir://电器名/动作` |
| TF 卡文件 | ✅ `files` 包：列目录、读、写（需确认），拒绝密钥与系统目录 | `file:///vat/...` |
| 定时与提醒 | ✅ `timer` 包：到点全屏提醒 + 提示音，关机期间错过的开机补发 | `timer://in/10m` `timer://at/18:30` `timer://list` |
| BLE 扫描 | ❌ 暂不做：Bluedroid 使 Flash 80%→96%、静态内存 +22 KB，会压垮 TLS 握手余量；以后可评估 NimBLE | — |

可交互的界面（画框、列表、选择、面板）留给 v0.6 的 `ui` 包。

不放进 v0.5：USB 口当键盘（HID，让 Cardputer 往电脑里打字），USB 目前只用于刷机，记在后续路线。

Cap LoRa-1262（GPS 与 LoRa 在同一个 Cap 上，可以同时工作）：

| 能力 | 状态 | target |
|:--|:--|:--|
| GPS（ATGM336H） | ✅ `gps` 包（TinyGPSPlus 1.0.3）：位置、海拔、在用/可见卫星、HDOP、UTC 时间、NMEA 计数；UART1 115200（G15 收/G13 发），2 KB 接收缓冲，主循环每次最多解析 512 字节；默认开启，“关闭”只停止读取（Cap 不给软件断 GNSS 电源）。遵循提示词已有的传感器约定 | `sensor://gps/location` `sensor://gps/satellites` `sensor://gps/time` |
| LoRa（SX1262） | 驱动与 `lora` 包已写，**待实测**（RadioLib 7.8.1）：默认关闭、芯片保持复位不占总线；`session connect lora://` 或 `write lora://settings/enabled on` 开始接收；收到的消息是编号清单（`read lora://messages`），存 `/vat/things/lora/inbox.jsonl`；`write lora://send` 需确认，且频点已设、天线已确认才会发射；`write lora://settings/frequency|power|sf|bandwidth|cr|antenna` | `lora://status` `lora://messages` `lora://send` |
| Grove 口（G1/G2） | ✅ `grove` 包：扫描 Ex_I2C，按地址推测型号 | `sensor://grove` |

设备档案（`/vat/things/`）——有持久数据的包都存这里，一个设备一个文件（目前已用：`ir/`、`timers/`；下面其余为设计）：

```text
/vat/things/
├── ir/<名称>.json     品牌、协议、房间、别名、当前状态（开关/模式/温度/风速）、验证记录
├── ble/known.json     起过名字的蓝牙设备：名字、MAC、类型、最后一次看到的时间与信号
├── lora/nodes.json    节点编号、昵称、最后一条消息；频点与功率设置
└── gps/places.json    命名地点：家、公司……
```

- **添加红外电器**：`session(action=create,target=ir://客厅空调)`，品牌从内置的常见电器目录里查候选协议，逐个发“开机”，每发一个问“空调有反应吗？Y/N”，按 Y 的那个协议记入档案并标为已验证。红外是单向的，设备不知道电器是否响应，所以确认只能由人来做。
- **状态**：空调遥控码每次都带完整状态，档案记住上一次的状态，`write ir://客厅空调/temp 26` 只改温度，其余沿用。
- **读写**：`read ir://` 编号列出所有电器，`read ir://客厅空调` 看状态；蓝牙“第 3 个是我的耳机”写进 `ble/known.json`。
- **带入对话**：像 SSH 主机名一样，每次请求带上设备名单和别名（“可见设备：客厅空调(ir，别名大厅空调)…”），模型按别名对上，不猜。
- **记忆**：状态变化写进归档，可以问“昨晚空调开了多久”；设备 Web 页可以查看和修改档案。
- 后续：红外的协议覆盖和确认流程在真实电器上测试后再优化。

LoRa 要先定的事：

- **频率**：这款 Cap 只支持 868–923 MHz，不含国内常用的 470–510 MHz。频点和功率由用户按当地规定决定、写进设置；没设置前只收不发。
- **天线**：没装天线绝不能发射。固件检测不到天线，首次发送前在确认框里提醒。
- **总线**：SX1262 与 TF 卡共用 SPI，驱动和存储共用一个 `SPIClass` 和总线锁；收包用 DIO1 中断，先进内存小队列，再由 worker 落卡。
- **对方立即回复会收不到**：SX1262 发完转接收约 7–10 ms，对端等约 30 ms 再回，或加长前导码。
## 即时 UI 与本地应用

让 Agent 用一句话生成小界面，并把它保存成可以反复打开的“本地应用”。本质是七个工具组合已注册的扩展包，不新增工具，也不在设备上执行代码。

- 界面描述：很小的 JSON（标题、编号行、选项、数值、刷新来源和周期），有大小上限和字段白名单，渲染器只画声明过的元素。
- `ui` 包：`write ui://panel/<名字>` 创建或更新界面，`read ui://panel/<名字>` 读取用户的选择；新增一个渲染页，键盘选择和确认。
- 本地应用：把界面描述连同用到的 target 和刷新周期保存下来，可以列出、打开、删除（`ui://apps`）。
- 例子：“做一个看开发机负载的小面板，每 30 秒刷新”= `exec ssh://开发机 uptime` + `write ui://panel/load`；“WiFi 选择器”= `read wifi://scan` + `write ui://panel/wifi`，选中后 `write wifi://SSID`（仍走确认与密钥输入）；“GPS 面板”= `read gps://local` + `write ui://panel/gps`。

前置条件：v0.5 的基础扩展包（面板要有数据来源）。
## 远程更新固件（OTA）

在开发机上编译出新固件后，设备通过已有 SSH 连接拉取 `.bin`，写入另一个程序分区后重启；新版本启动失败时自动回滚，`/vat` 数据不受影响。做成 `firmware` 包：`read firmware://version`；`write firmware://ssh/开发机/<路径>.bin`（需要确认，录音期间不可执行）。

## 物联网适配器

候选协议：MQTT、Home Assistant、ESP-NOW、HTTP、BLE GATT、串口。映射示例：

```text
读取书桌温度    → read(target="sensor://desk/temperature")
打开房间一号灯  → write(target="device://room/light-1/power", content="on", confirm_required=true)
```

适配器负责认证、发现、协议转换、状态回读和失败恢复；模型只产生计划，不直接接触总线或凭据。

## 防御性安全诊断

只面向用户拥有或明确授权的资产：

- 默认只读：ping、DNS、路由、端口状态、TLS 证书、HTTP 头、本机网络状态。
- 需要确认：端口扫描、服务指纹、抓包、BLE/Wi-Fi 主动探测、较高频率的资产发现。
- 不纳入：撞库、凭据窃取、恶意载荷、未授权漏洞利用、Wi-Fi deauth、规避审计。

执行器必须校验目标在授权清单内，限制速率和参数，记录完整操作日志，并允许物理中断。

## 4G 联网（ML307R）

计划最后做（用户正在采购模块）。

- **接线**：Grove 口就是普通 IO（G1/G2），直接配成串口，不需要改装或转 I2C：G2→模块 RXD、G1←模块 TXD、5V、GND。ESP32-S3 的 3 个硬件串口：GPS 用 1 个，4G 用 1 个。
- **采购前确认**：① 模块板自带 DC-DC 稳压和大电容（Cat.1 发射有安培级尖峰，Cardputer 的 5V 未必扛得住，必要时单独供电）；② 串口是 3.3 V TTL（很多 4G 模块是 1.8 V，要带电平转换）；③ AT 手册支持 PPP 拨号；④ 天线、SIM 卡与流量。
- **软件**：用 PPP（Arduino 3.x 自带 PPP 库）让 4G 成为第二张网卡，WiFi 优先、断开时自动切 4G，HTTPS、SSH、联网搜索都不用改；不走模块自带的 AT 版 TCP/HTTPS。
- **风险待实测**：Grove 只有两根信号线，没有硬件流控和复位线（只能 AT 软复位）；PPP 额外占用内存，与 TLS 握手内存下探一起评估。
- **`cellular` 包**：`read cellular://status`（信号、运营商、在线）；短信 `read sms://`、`write sms://号码`（发送需确认）。

## 接入顺序

1. ✅ SSH、远程 TUI、Coding Agent 主链路。
2. ✅ 一个只读本机能力（Wi-Fi 扫描）和一个需要确认的写操作（语音设置 Wi-Fi），验证 `read` 路由、`write` 确认和状态回读闭环。
3. ✅ 长上下文（TF 卡）与联网搜索。
4. ✅ 基础扩展包：本机能力补全，外接 GPS、LoRa 等模块（LoRa 收发待实测）。
5. 即时 UI 与本地应用。
6. 远程更新固件（OTA）。
7. MQTT 或 Home Assistant 适配器。
8. 已授权的防御性网络诊断，配套目标白名单、参数策略和审计测试。
9. 4G 联网（ML307R）。

任何扩展只有在“计划 → 确认 → 执行 → 观察 → 验证 → 记录”整个闭环的 E2E 通过后，才能进入默认能力。
