# 双屏语音操作 Agent 终端产品设计

状态：V1 已在 Cardputer 上单机实现（2026-11-03）；本文仍是产品边界与设计原则的依据。

## 实现目录

- 本仓库是 Cardputer 实现：[构建、刷写、按键](../README.zh-CN.md)、[文档导航](README.zh-CN.md)、[自动验收](../tools/vat_e2e.py)。
- 其他硬件（3DS、Switch）以后在工作区 `voice-agent-terminal/` 下与 `cardputer/` 同级各建一个仓库，同样以单机运行为目标，实现以本文的产品边界和设计原则为准。

## 产品定义

V1 是 Cardputer 单机运行：设备自身完成录音、语音识别、七工具 Agent、SSH、远程 tmux/Codex/Pi 与界面，配置和 Key 保存在设备存储，通过设备自带的 Setup 页和配置网页填写。运行时不依赖任何电脑进程。实现见 [单机架构](ARCHITECTURE.zh-CN.md)。

这是一个双屏、带物理键盘和麦克风的便携式个人操作终端。用户通过语音或文字表达目标，本机操作 Agent 将目标转换成可观察、可确认、可中断的设备操作、SSH 操作和远程 Agent 操作。

产品不是语音键盘、普通 SSH 客户端，也不是在小型硬件上重新实现一个 Coding Agent。它的核心职责是理解“用户想做什么”，选择正确的主机、项目、会话和远程工具，并可靠地完成连接、启动、输入、等待、批准、接管和恢复。

涉及软件开发时，本机操作 Agent 不负责直接阅读和修改大量代码，而是连接远程电脑，把开发目标交给 Codex、Claude Code、Pi 等专业 Coding Agent。本机负责调度和监督，远程 Coding Agent 负责具体开发。

核心闭环是：

```text
用户表达目标
→ 本机 Agent 理解意图
→ 转换成小而明确的操作
→ 通过 SSH 或本机能力执行
→ 观察真实结果
→ 必要时请求用户批准
→ 继续执行或交给远程 Agent
→ 验证结果并汇报
```

## 产品形态

推荐采用翻盖式双屏硬件，下半部分带紧凑物理键盘、麦克风、方向控制和独立操作键。

```text
┌──────────────────────────────────┐
│ SSH                              │
│                                  │
│ user@server:~/project$           │
│ $ codex                          │
│                                  │
│ 远程 PTY / tmux / Codex / Vim    │
└──────────────────────────────────┘
                 铰链
┌──────────────────────────────────┐
│ AGENT · dev-server · ~/project   │
│                                  │
│ 你：继续处理昨天的登录问题       │
│ ✓ 已连接开发机                   │
│ ✓ 已恢复 Codex 会话              │
│ ● Codex 正在运行测试             │
│ ! 等待批准                       │
│                                  │
│ [批准] [拒绝] [停止]             │
├──────────────────────────────────┤
│       紧凑 QWERTY 键盘           │
└──────────────────────────────────┘
```

建议硬件组成：

- 上屏：SSH 和远程应用现场。
- 下屏：本机 Agent、语音转写、任务进度和审批。
- 物理 QWERTY 键盘：精确输入、修正转写和人工接管。
- 麦克风：按键录音，不在 MVP 中持续监听。
- 扬声器：完成、失败和审批提醒，可选语音播报。
- 方向键或摇杆：终端滚动、菜单选择和 TUI 操作。
- 独立按键：Agent 语音、文字听写、批准、拒绝、中断和焦点切换。
- Wi-Fi、USB-C 和本地存储。

3DS 适合验证双屏交互；Cardputer 适合验证便携键盘、麦克风和小屏交互；Switch 适合验证较完整的双工作区 UI。最终专用硬件可以结合三者优点，并使用能够稳定运行 SSH、音频、持久状态和 Agent 编排的 Linux 平台。

## 两个独立工作空间

两块屏幕代表两个平级但相互协作的世界，不能混成同一个输出流。

### 本机 Agent 屏幕

下屏负责人与本机操作 Agent 的协作：

- 接收语音和文字目标。
- 展示语音转写并允许修改。
- 显示当前主机、工作区、远程会话和输入目标。
- 显示简洁的操作时间线和工具结果摘要。
- 显示真实的审批对象、参数和风险。
- 提供批准、拒绝、中断和人工接管。
- 保存任务上下文、操作记录和记忆。

下屏不展示模型的隐藏推理过程，只展示用户能够验证的目标、动作、结果和下一步。

### SSH 屏幕

上屏负责远程现场：

- 原始 SSH PTY 和 ANSI/VT100 渲染。
- Shell、tmux、Vim、REPL 和日志。
- Codex、Claude Code、Pi 等远程 TUI。
- Agent 操作造成的真实系统变化。
- 用户随时进行人工输入和接管。

Agent 的解释和任务摘要不写入 SSH 屏幕；普通诊断命令优先使用独立 SSH exec channel，避免污染用户正在使用的 PTY。

### 独立状态机

Agent 工作区：

```text
IDLE
→ LISTENING
→ TRANSCRIBING
→ REVIEW
→ PLANNING
→ OPERATING
→ WAITING_APPROVAL
→ VERIFYING
→ DONE / FAILED / CANCELLED
```

SSH 工作区：

```text
DISCONNECTED
→ CONNECTING
→ TERMINAL_BROWSE
→ TERMINAL_INPUT
→ DISCONNECTED
```

Agent 等待审批时，SSH 仍可滚动查看；SSH 断开时，Agent 对话和任务记录仍然保留。

## 两种语音通道

语音必须支持“转操作”和“直接转文字”，并明确区分，不能完全依赖模型猜测。

### Agent 操作模式

语音作为目标，由本机 Agent 转换成操作。

示例：

> 连接开发机，打开岛民项目，让 Codex 检查测试失败。

处理过程：

```text
录音
→ ASR
→ 用户确认转写
→ 意图解析
→ 连接 SSH
→ 找到项目和持久会话
→ 启动或恢复 Codex
→ 提交开发任务
```

### 文字听写模式

语音仅转成字面文字并插入当前输入目标，不理解、不拆解、不执行。

输入目标可以是：

- 本机 Agent 输入框。
- SSH Shell。
- Codex、Claude Code 或 Pi 的 TUI 输入框。
- Vim、REPL 或其他远程程序。
- 本地便笺。

默认只插入文字，不自动发送 Enter。即使转写内容是一条危险命令，也不能因为听写而自动执行。

### 交互建议

```text
按住 MIC       → Agent 操作模式
按住 L + MIC   → 文字听写模式
```

录音结束后也可以统一进入审核：

```text
“让 Codex 先修复 API，不要改数据库。”

[作为操作] [输入为文字] [编辑] [丢弃]
```

界面始终显示当前模式、输入目标和是否会自动提交。

## 本机操作 Agent 与远程 Coding Agent

本机 Agent 是 Meta Agent 或操作调度 Agent，不是 Coding Agent。

### 本机操作 Agent 的职责

- 理解用户想做什么。
- 解析主机、项目、应用和会话别名。
- 选择合适的本机或远程能力。
- 建立、恢复和管理 SSH 连接。
- 创建或恢复 tmux 等持久会话。
- 启动、恢复和操作远程 Agent。
- 转发远程 Agent 的审批请求。
- 管理用户与 Agent 的终端控制权。
- 在断线、超时和失败后恢复。
- 压缩长任务状态并向用户汇报。

### 远程 Coding Agent 的职责

- 阅读和搜索代码。
- 修改文件。
- 执行构建、测试和调试。
- 处理具体的软件开发任务。
- 通过结构化事件或 TUI 报告进度和审批请求。

典型调用链：

```text
用户
→ 本机操作 Agent
→ SSH / tmux
→ Codex、Claude Code 或 Pi
→ 远程项目与工具
```

## 小而精确的操作工具

本机 Agent 应使用小而明确的原子工具，不应主要生成一整段不可检查的 Shell。

Agent Runtime V2 的可见工具固定为七个，不为 SSH、tmux、TUI 或不同 Agent 分别复制一套名称：

```text
discover(scope, query, target?, limit?)
read(target, max_bytes?)
write(target, content, mode, confirm_required)
exec(target, program, args, cwd?, confirm_required)
observe(target)
input(target, text?, keys?, submit)
session(action, target?, provider?, project?, prompt?, policy_mode?)
```

`target` 使用明确的资源地址区分底层能力：

```text
ssh://devbox
ssh://devbox/etc/os-release
tui://current
tmux://devbox/work
agent://devbox/codex/session-1
device://room/light-1/power
sensor://desk/temperature
config://wifi/home
memory://recent-tasks
```

SSH 连接、文件读取、远端命令和 TUI 输入只是这七个原子调用的不同路由，不是新的 Agent 工具。底层适配器可以很多，但模型看到的操作面保持稳定。

ESP、MQTT、Matter、Home Assistant 或自定义串口设备也遵守同一规则：适配器负责把 `read/write/exec` 翻译成 GPIO、总线或设备协议，Agent 不获得一套新的“灯工具”“传感器工具”。

当前实现顺序先完成 SSH、远程 TUI、tmux 和 Coding Agent。物联网、Cardputer 硬件资源和防御性安全诊断保存在 [扩展方向](EXTENSIONS.zh-CN.md)，不进入当前 MVP 的实现范围。

首版能力现已通过正式扩展注册表装载：SSH 传输、TUI 交互、远程 Agent 生命周期，以及 Linux/Git/Docker/systemd 能力目录均为独立包；`discover` 和实际执行共用同一个注册表，开发约定见 [扩展包开发手册](EXTENSION_PACKAGE_GUIDE.zh-CN.md)。

`exec` 必须使用程序名和参数数组，避免默认经过 Shell。写入、删除、重启、sudo、权限修改和不确定操作必须标记 `confirm_required=true`。大型代码修改继续委托给远程 Coding Agent，而不是由本机 Agent 展开大量文件编辑步骤。

## 远程 TUI 输入与控制

本机 Agent 可以通过 SSH PTY 向 Codex 等 TUI 输入文字和按键，但必须形成“观察、判断、输入、验证”的闭环，不能盲打。

建议提供：

```text
tui_observe(session_id)
tui_input(session_id, text, submit)
tui_key(session_id, key)
tui_wait(session_id, condition, timeout)
tui_interrupt(session_id)
```

支持的按键至少包括：

```text
ENTER ESC TAB
UP DOWN LEFT RIGHT
PAGE_UP PAGE_DOWN
CTRL_C CTRL_D BACKSPACE
```

SSH 客户端本身已经解析 ANSI/VT100，因此 `tui_observe` 应读取终端字符网格、光标位置和清理后的可见文本，而不是依赖截图 OCR。

### TUI 适配器

```text
TuiAdapter
├── CodexAdapter
├── ClaudeAdapter
├── PiAdapter
├── ShellAdapter
└── GenericAdapter
```

适配器把不同程序归一化为：

```text
READY
RUNNING
APPROVAL
SELECTION
COMPLETED
ERROR
UNKNOWN
```

对于未知状态，停止自动输入并请求用户确认。

第一阶段可以通过 PTY 和 tmux 驱动所有远程 CLI；后续优先使用各远程 Agent 提供的 JSON、RPC 或 app-server 接口，将命令、进度、结果和审批转换成统一事件。PTY 始终保留为人工观察和兼容后路。

## 终端控制权

用户和 Agent 不能同时向同一个 PTY 写入。每个终端会话具有明确的控制权：

```text
CONTROL_AGENT
CONTROL_USER
CONTROL_LOCKED
```

规则：

- Agent 控制时，下屏明确显示状态。
- 用户按下物理键即可暂停 Agent 并人工接管。
- 用户交还控制权后，Agent 必须重新观察终端，不能沿用旧状态继续输入。
- 审批界面出现时锁定普通字符，避免误输入。
- 同一时间只允许一个 writer 写入 PTY。
- 长文本优先使用 bracketed paste，并检查 UTF-8、回显和提交结果。
- 发送失败后不得自动重复提交，避免同一任务执行两次。

## 权限与审批

模型提出操作不等于获得执行权限。权限由本机设备掌握，并绑定当前主机、工作区、任务和工具类别。

建议默认策略：

| 操作 | 默认策略 |
|---|---|
| 查看状态、观察终端、读取普通信息 | 允许 |
| 建立已保存的 SSH 连接 | 允许 |
| 创建或恢复普通 tmux 会话 | 允许 |
| 向远程 Agent 发送自然语言任务 | 允许或任务级确认 |
| 向 Shell 或未知 TUI 发送输入 | 询问 |
| 普通命令执行 | 询问或任务级允许 |
| 文件写入、部署、安装和网络变更 | 询问 |
| sudo、删除、覆盖、reset | 默认拒绝或强确认 |

审批卡必须展示真实操作：

```text
目标：dev-server
目录：/srv/islander
来源：Codex
操作：执行数据库迁移
命令：具体命令或结构化参数
风险：将修改数据库结构

[允许一次] [本次任务允许] [拒绝]
```

永久规则只能在设置中修改，不能通过一次误按创建。

## 记忆设计

产品最有价值的记忆不是保存所有聊天，而是理解用户的操作世界。

### 会话记忆

- 当前目标和计划。
- 最近对话。
- 已执行操作和结果摘要。
- 当前主机、工作区和远程 Agent 会话。
- 临时授权和待审批事项。

### 主机和项目记忆

```text
“开发机” → ubuntu@dev.example.com

“岛民项目”
主机：开发机
目录：/srv/islander
默认 Agent：Codex
默认会话：tmux:islander
```

### 用户偏好和策略

```text
开发任务默认使用 Codex
长任务默认放入 tmux
部署生产环境必须询问
数据库迁移必须询问
普通测试可以任务级允许
任务完成后给出简短摘要
```

### MVP 记忆范围

第一版只保存：

```text
当前任务
最近对话
工具调用和结果摘要
主机与项目别名
远程会话标识
明确记录的用户偏好
```

长期记忆优先由用户通过“记住……”显式创建。后续再增加自动总结、检索和遗忘策略。

## 统一事件协议

不同硬件和远程 Agent 应共享统一事件协议，便于在 3DS、Switch 等新硬件上复用同一套任务、审批与展示语义。Cardputer V1 的任务记录（`/api/job`）与操作日志（`operations.jsonl`）就是这套协议的第一个落地形态。

示例 JSONL：

```json
{"type":"transcript","text":"继续处理昨天的登录问题"}
{"type":"status","phase":"connecting","target":"dev-server"}
{"type":"operation","name":"tmux_attach","summary":"恢复 islander 会话"}
{"type":"remote_agent","provider":"codex","state":"running"}
{"type":"approval","id":"a1","risk":"database","summary":"Codex 请求执行数据库迁移"}
{"type":"message","text":"API 修复完成，相关测试已通过。"}
{"type":"done","ok":true}
```

协议需要支持：

- 任务 ID 和远程会话 ID。
- 状态、操作、结果、审批、错误和完成事件。
- 中断与取消。
- 断线续传或重新查询状态。
- 敏感字段过滤。
- 有界输出，避免终端日志填满模型上下文和小屏 UI。

## 典型使用场景

用户说：

> 连到开发机，打开岛民项目，让 Codex 看看登录失败的问题，修好后跑测试，但数据库迁移先问我。

本机操作 Agent：

```text
1. 将“开发机”解析为保存的 SSH 配置。
2. 将“岛民项目”解析为远程目录和默认 Agent。
3. 建立 SSH 连接。
4. 查找并恢复 tmux:islander；不存在则创建。
5. 启动或恢复 Codex。
6. 把用户目标和限制提交给 Codex。
7. 监听进度、结果和审批请求。
8. 将数据库迁移请求显示在下屏。
9. 根据用户决定向远程 Agent 发送批准、拒绝或补充要求。
10. 任务完成后汇总结果，保留可继续的远程会话。
```

上屏始终显示远程真实现场；下屏显示压缩后的操作时间线和用户需要处理的决定。

## MVP 范围

第一版目标是跑通完整闭环，而不是实现复杂人格、自动长期记忆或本地大模型。

### 必须实现

- 双工作区状态模型。
- SSH 公钥连接、PTY、终端渲染和断线处理。
- 独立 SSH exec channel。
- 按键录音、ASR 和转写审核。
- Agent 操作与文字听写两种语音模式。
- 主机与项目别名。
- tmux 会话查找、创建和恢复。
- 启动、恢复和输入一个远程 Coding Agent。
- TUI 观察、文字输入、按键输入和中断。
- 用户与 Agent 的终端控制权切换。
- 最小审批界面和审计记录。
- 当前任务、最近会话和显式记忆。

### 暂不实现

- 唤醒词和持续监听。
- 完整离线 ASR 和本地大模型。
- 自动生成大量长期记忆。
- 同时编排很多远程 Agent。
- 对所有 TUI 的通用自动识别。
- 让本机 Agent承担复杂代码修改。
- 无限制自主执行。

## MVP 验收场景

用户通过语音说：

> 连接开发机，进入 islander 项目，启动 Codex，让它检查当前测试失败，完成后告诉我。

验收标准：

1. 正确识别语音并允许用户修改。
2. 正确找到主机和项目目录。
3. 自动建立 SSH 连接。
4. 自动创建或恢复 tmux 会话。
5. 自动启动或恢复 Codex。
6. 将任务准确输入 Codex TUI。
7. 上屏显示真实远程终端。
8. 下屏显示连接、会话、运行和完成状态。
9. 用户可以追加听写、人工接管或中断。
10. 断线重连后可以恢复远程任务。
11. 遇到高风险操作时请求批准。
12. 完成后给出简洁结果摘要。

## 实施顺序

### 阶段一：操作协议与模拟器

- 定义统一任务、操作和事件协议。
- 实现假 SSH、假 TUI 和假远程 Agent。
- 验证状态机、审批、取消和控制权切换。

### 阶段二：真实 SSH 与 tmux

- 接入 SSH PTY 和独立 exec channel。
- 实现终端观察和输入。
- 实现 tmux 会话管理和断线恢复。

### 阶段三：远程 Agent 适配

- 首先选择一个远程 Agent 跑通 PTY 适配。
- 实现启动、恢复、输入、观察和中断。
- 加入审批识别和保守的 UNKNOWN 状态。
- 后续再接入结构化 JSON 或 RPC 适配。

### 阶段四：语音与双屏硬件

- 接入录音和 ASR。
- 实现 Agent 操作和文字听写两条语音通道。
- 在 DSSH、NSSH 或 Cardputer 上验证交互。
- 根据实机体验确定最终硬件尺寸和按键布局。

### 阶段五：记忆与多设备一致性

- 保存主机、项目、会话和用户策略。
- 支持“继续昨天的任务”。
- 使用统一协议连接不同硬件客户端。

## 现有项目的利用方式

### DSSH

DSSH 已经具备 3DS 麦克风录音、SSH 辅助 channel、语音转写、终端和下屏 UI，可以最快验证双屏语音操作闭环。下一步应把单轮问答升级为操作事件、远程 TUI 控制和审批。

### NSSH

NSSH 已经具备较完整的 SSH 终端、输入模式、中文软键盘和双栏规划，适合验证更完整的 Agent 时间线、审批和远程 Agent 管理。需要补充独立 exec channel、语音和 TUI 适配。

### Cardputer

Cardputer 的键盘、麦克风、扬声器和便携尺寸适合验证产品的随身形态。V1 已证明它可以单机完成全部链路：语音识别和模型推理通过 HTTPS 直连云端，Agent 编排、Policy、SSH 与 tmux 适配都在 ESP32-S3 上运行。屏幕只有一块，双工作区用 `Btn0` 切页实现。

## 核心设计原则

1. 本机 Agent 负责“做什么”，远程专业 Agent 负责“怎么开发”。
2. Agent 工作区和 SSH 工作区平级、独立、始终可相互观察。
3. 语音操作和文字听写是两条不同通道。
4. 所有自动操作都要可观察、可确认、可中断和可恢复。
5. 优先使用小而明确的工具，不依赖不可审计的大段 Shell。
6. 操作 TUI 必须先观察，再输入，再验证，不能盲打。
7. 用户随时拥有终端最高控制权。
8. 高风险操作显示真实目标和参数，不只显示自然语言摘要。
9. 记忆优先保存主机、项目、会话和用户策略，而不是无限保存聊天。
10. 先跑通端到端闭环，再增加人格、复杂记忆和自主性。

最终产品可以概括为：一个通过语音和自然语言，把用户想法转换成设备操作、SSH 操作和远程 Agent 任务的双屏个人操作终端。

进一步设计：

- [架构](ARCHITECTURE.zh-CN.md)
- [Agent 运行时：多步任务、发现、连接与权限](AGENT_RUNTIME.zh-CN.md)
- [扩展包开发手册（操作包、资源与提示）](EXTENSION_PACKAGE_GUIDE.zh-CN.md)
- [后续扩展场景与安全边界](EXTENSIONS.zh-CN.md)
- [人工验收清单](MANUAL_ACCEPTANCE.zh-CN.md)
- [配置与秘密存储](CONFIGURATION.zh-CN.md)
- [完整文档导航](README.zh-CN.md)

## 许可证

项目源码和项目自有文档按 [MIT License](../LICENSE) 发布。依赖库、模型和外部服务仍遵循各自许可证与服务条款。
