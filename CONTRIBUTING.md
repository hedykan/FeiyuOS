# 贡献指南

Voice Agent Terminal 只有一种运行形态：**Cardputer 单机运行**。运行时不依赖任何电脑进程；电脑只用来编译、刷写和跑验收脚本。不接受需要电脑常驻进程、串口桥接或兼容层的功能。

## 必读规范

| 文档 | 什么时候读 |
|:--|:--|
| [扩展包开发手册](docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md) | 新增或修改任何设备能力。**新能力只能以扩展包的方式加入，模型侧七个工具不变** |
| [固件开发约束](docs/FIRMWARE_RULES.zh-CN.md) | 改动任何固件代码。内存、音频、存储、线程、构建的硬规则 |
| [操作 Agent 运行时](docs/AGENT_RUNTIME.zh-CN.md) | 改动提示词、工具循环、Policy、审批、对话记忆 |
| [单机架构](docs/ARCHITECTURE.zh-CN.md) | 了解任务划分和源码结构 |
| [E2E 验收](docs/E2E.zh-CN.md) | 提交前验证 |
| [扩展方向](docs/EXTENSIONS.zh-CN.md) | 挑选要做的能力 |

## 开发流程

克隆后先启用提交钩子（每次提交前用 [gitleaks](https://github.com/gitleaks/gitleaks) 扫描暂存的改动，发现密钥、token、私钥就拒绝提交；没装 gitleaks 时也拒绝）：

```sh
git config core.hooksPath .githooks
```

1. **从 issue 或提议开始**。新能力先说明：用哪个 target scheme、哪些工具动作、哪些操作需要确认、怎么验收。
2. **在分支上开发**（`feat/<能力>`、`fix/<问题>`），不直接提交到 `main`。
3. **按范式实现**：
   - 新能力：`src/packages/<名字>_package.cpp`，加上 `builtin.cpp` 里的一行注册；
   - 底层硬件或协议适配放在 `src/ops_*.cpp`；
   - 模型需要知道新 target 的用法时，在包里写 `usage()`（见手册“模型怎么知道用法”），先用提示词实验台验证。
4. **先在开发机上测，再刷到实物**：
   ```sh
   make host-test                                  # 几秒：可移植源码对着录制的真实返回跑
   make flash PORT=/dev/ttyACM0 DEVICE=http://<设备 IP>   # 编译、刷写、等到设备重启就绪
   ```
   - 解析、格式化、文本处理这类不碰硬件和网络的逻辑写成纯函数（如 `place_format.cpp`），放进 `tests/host/`，用 `make host-test` 验证；能在开发机上发现的问题不要靠刷机发现。
   - 一轮刷机约 2 分钟，修复先攒齐（同类问题全量排查一遍）再刷一次，不要“改一处刷一次”。
   - 刷写后用 `make flash` / `tools/wait_ready.sh` 等就绪，不写固定的 `sleep`。
   - 同步源码到构建机时不要保留时间戳（见固件开发约束 §7）。
5. **跑 E2E**：默认 `--phases auto` 只跑 S0 和受影响的阶段（对照表见 E2E 文档“跑多少”）；全量只在发版前跑。新包要在 `tools/vat_e2e.py` 里补用例（手册 §6）。
   ```sh
   make e2e DEVICE=http://<设备 IP> PHASES=s0,s6
   ```
   发布前跑一次 `PHASES=all`（全量），结果放进 `docs/evidence/`。
6. **人工验收**：涉及按键、录音、屏幕、物理设备或断电的改动，按 [人工验收](docs/MANUAL_ACCEPTANCE.zh-CN.md) 补对应步骤。
7. **提交**：使用 Conventional Commits（`feat(wifi): …`、`fix(audio): …`、`docs: …`），一个提交只做一件事。
8. **提 PR**：按 PR 模板填写，贴上 E2E 结果摘要。

## 评审会逐条检查

- 没有新增第八个工具，没有改工具 schema。
- 新 target 由唯一的包认领，和其他包的认领范围不重叠；未知 target 不会被猜测执行。
- 写入、删除、提权、重启、切换网络、物理动作都要经过 Policy 确认。
- 密钥只通过 `needs_secret()` 加设备输入框获取，从不读取模型参数里的密码；E2E 证明密钥不落盘。
- 新包提供 `hints()`，帮助页能看到它。
- 远端命令只用 argv 组装，凭据不进入模型上下文、日志和结果。
- 符合固件开发约束：不在 RAM 拼大对象、TLS 和 SSH 不同时存在、输出限长、录音期间不写 Flash、I/O 有超时。
- E2E 在实物上通过，并且没有非预期重启（`no_unexpected_reboot`）。
- 文档同步更新：手册 §7 的包一览、扩展方向的状态、配置说明。

## 不提交的东西

API Key、Wi-Fi 密码、SSH 私钥、主机地址、语音录音、设备 `/vat` 的拷贝、本地 `logs/`。

提交贡献即表示你有权提交该内容，并同意按项目的 MIT License 发布。
