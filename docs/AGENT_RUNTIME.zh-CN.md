# 操作 Agent 运行时

运行时在 Cardputer 的 worker 任务里执行（`src/agent.cpp`）。它把一句话变成最少的原子操作，自己不写代码；软件开发类目标交给远程 Codex/Pi/Claude。

## 七个工具

模型只看到这七个工具，schema 与系统提示词在 `src/agent_prompt.h`：

| 工具 | 用途 | target 示例 |
|:--|:--|:--|
| `discover` | 渐进发现资源、能力包、命令、命令帮助 | `scope=resources/capabilities/commands/help` |
| `read` | 读资源 | `ssh://开发机/etc/hostname`、`device://local/status`、`memory://recent-tasks` |
| `write` | 写资源（`mode=create/overwrite/append`） | `ssh://开发机/tmp/note.txt`、`device://local/display/brightness` |
| `exec` | 执行一个程序，`program` + `args`，可带 `cwd`，不经 shell 拼接 | `ssh://开发机` |
| `observe` | 观察终端 / TUI | `tui://current` |
| `input` | 向终端输入文字或按键（`keys=[CTRL_C]`、`submit`） | `tui://current` |
| `session` | 连接、tmux 会话、远程 Agent、审批、策略 | `ssh://开发机`、`tmux://开发机/work`、`agent://current`、`wifi://家里 WiFi` |

## 一个任务

一句话就是一个任务。执行流程是：模型调用工具，工具结果回传给模型，模型决定下一步，如此循环，直到给出文字回复：

```text
转写文本 + 最近对话（TF 卡上的上下文窗口，长度由 `ui.context_kb` 决定，按 tool_calls/tool 原生结构保存）
        │
        ▼
循环（最多 6 个操作步骤；discover 不计步骤但总数 ≤2）
  ├─ 模型回工具调用（每轮 ≤3 个）→ 逐个过 Policy
  │     allow           → 执行，结果回传
  │     deny            → 拒绝说明回传
  │     ask             → 暂停：等 Y / N / A
  │     需要密钥        → 暂停：等设备上的掩码输入
  ├─ 模型回文字        → 这就是最终总结，任务结束
  └─ 到达 6 步          → 强制要求模型用文字总结
        │
        ▼
整个任务写入对话记忆；每一步写入 operations.jsonl；时间线记录结论与步骤
```

**暂停与继续**：暂停时，任务的上下文（已完成的轮次、当前轮还没执行的调用）保存在内存里，屏幕上这一行变成橙色。
- **Y**：执行这一步，然后自动继续；
- **N**：告诉模型“用户拒绝了这一步、未执行”，由模型决定换一种做法还是结束；
- **A**：执行这一步，本任务后续需要确认的步骤也全部自动通过。凭据路径照样硬拒绝，需要密钥的步骤照样要输入；
- **`` ` ``（Esc）**：放弃整个任务。

**密钥**：扩展包的 `needs_secret()` 认为需要密码时，任务暂停并弹出掩码输入框。输入的值只交给这一步的 `execute_secret()`，不进入模型上下文、对话记忆、操作记录、时间线和日志；取消输入时，这一步记为未执行，任务继续。模型的系统提示规定它不得索要、复述或在参数里填写密码。

**清单与指代**：设备产生的清单（WiFi、主机、tmux 会话、能力包、命令）都是带编号的紧凑文本；模型向用户展示任何清单时也要按编号、按工具结果的顺序列出。用户说“第 N 个”时，指最近一次展示的编号清单；找不到或者越界就先澄清。模型自己的编号总结也会完整存进对话记忆，所以即使原始输出被截断，编号仍然可以引用。

对话记忆按 API 原生结构保存：模型调用了工具，就记一条 `assistant.tool_calls` 加对应的 `tool` 结果，最后一条是 assistant 的总结。早期版本把工具结果渲染成 assistant 文字存进历史，模型会照抄“需要确认（尚未执行）”这类设备文案而不再调用工具——屏幕显示待确认，实际上什么也没挂起。

模型请求使用 `tool_choice=auto`（最后一步用 `none`）、`temperature=0`、`reasoning_effort=none`。HTTP 传输错误、5xx 和空回复自动重试一次。按 Esc 会把正在使用的 socket 直接 `shutdown`，当前阶段立即结束。

## Policy 与审批

`src/policy.cpp`，按 target 前缀记录会话级模式 `deny / ask / allow / allow_all`，默认 `allow`。

| 情况 | 决策 |
|:--|:--|
| `discover` | allow |
| 参数里出现凭据路径（`/.ssh/`、`/etc/shadow`、Key 文件、`secrets.json` 等） | deny，`allow_all` 和人工批准都不能放行 |
| `read`、`observe`、`input`、普通 `session` | allow |
| `exec` 白名单只读命令（`whoami`、`df`、`git status/log/diff`、`docker ps/logs`、`systemctl status` 等）且 `confirm_required=false` | allow |
| 其他 `exec` | ask |
| 本机可逆设置（亮度、息屏、音量、剪贴板、记忆笔记） | allow |
| 其他 `write`、`session set_policy/approve/reject` | ask |

ask 的操作不会执行。时间线上这一行变成橙色，显示主机、命令和“Y 执行 N 取消 A 后续都执行”，处理方式见上文“暂停与继续”。

## 能力包（discover 与路由）

所有能力都是注册在 `src/registry` 里的扩展包，`discover` 和执行共用同一份注册表：

| 包 | 认领范围 |
|:--|:--|
| `local` | 本机状态、设置、剪贴板、记忆（`device://local/*`、`memory://*`） |
| `wifi` | Wi-Fi 扫描、状态、切换、语音设置新网络（`wifi://`） |
| `ssh` | 远端文件与命令、连接状态（`ssh://`） |
| `tmux` | 终端观察与输入、会话管理（`tui://` `agent://` `tmux://`） |
| `agents` | Codex / Pi / Claude 的只读启动、恢复、中断、审批键 |
| `policy` | 目标权限策略 |
| `linux` `git` `docker` `systemd` | 命令目录：检索词、风险等级、远端可用性（`command -v`）、`--help` 摘要 |

执行时按注册顺序询问每个包的 `supports()`，由第一个认领的包执行；没有包认领时返回“没有能力包可处理 …”，不猜测。新增能力的方法和规则见 [扩展包开发手册](EXTENSION_PACKAGE_GUIDE.zh-CN.md)。

## 模型上下文由什么组成

每次请求依次是：

1. 系统提示：`agent_prompt.h` 的核心规则（角色、七工具、澄清、确认、密钥、清单、回复格式、物联网约定），后面接各扩展包的 `usage()`（按注册顺序）；
2. 可见 SSH profile 名称（只有名字，没有主机、用户和凭据）；
3. 当前案卷的要点和最近发现（有案卷时）；
4. 更早对话的摘要（`context/summary.json`，压缩后才有）；
5. 上下文窗口（`context/window.jsonl`，原生 `tool_calls` / `tool` 结构，TF 卡上 48 KB，无卡 8 KB）；
6. 当前转写，以及本任务已经执行的步骤和结果。

3–5 都从存储流式发送，不在内存里拼接。工具结果超过 4 KB 时，对话里只有开头 2 KB 和 `out#N`，完整内容在卡上，模型用 `memory://out/N?page=` / `?grep=` 取。任务结束后窗口超预算时，`agent::maintain()` 把较旧部分合并进摘要。详见 [扩展方向 · 长上下文](EXTENSIONS.zh-CN.md#长上下文tf-卡-v04)。

资源细节不常驻在提示里，模型需要时通过 `discover(resources|capabilities|commands|help)` 按需获取。

## 远程 TUI

`src/ops_tui.cpp` 通过 SSH 在远端操作 tmux：

- 启动：在远端查找 `codex`/`pi`/`claude`（含 nvm 目录），校验项目目录，新建 `vat-<provider>-<project>` 会话。Codex 用 `--sandbox read-only -a never`，Pi 用只读工具集，Claude 用 `--permission-mode plan`；带任务时等待界面就绪后粘贴并回车。
- 观察：`tmux capture-pane -p -J -S -100`；Feed 视图去掉横线、欢迎语和状态栏，保留最近 9 行。
- 输入：`set-buffer` + `paste-buffer` 粘贴文字（中文安全），按键用 `send-keys`。一次输入合成一条远端命令。
- 当前会话写入 `/vat/memory/tui.json`，重启后仍可直接观察和输入；远端会话不存在时自动遗忘，SSH 页提示重新连接。
- **终端由 Agent 管理**：`session connect ssh://主机` 接入（或新建）该主机上的 `vat-shell`（`PS1='$ '` 的 bash，47×11）；`start_agent` 切到 Codex/Pi/Claude 会话；`disconnect` 释放。固件本身不会自行打开终端。

## SSH

`src/ops_ssh.cpp` 使用 libssh：私钥或密码认证，连接在一次任务内复用，任务结束即关闭以释放内存。所有远端命令由 argv 逐个单引号转义后拼成；`cwd` 通过固定的 `sh -c 'cd -- … && exec …'` 包装。写文件内容走 stdin，不进入命令行。
