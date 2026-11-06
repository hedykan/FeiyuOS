# 扩展包

设备的每一项能力都是这里的一个包：它认领一组 URI（或 `session` 的动作），由七个通用工具调用。新增能力 = 新建 `<名字>_package.cpp` + 在 `builtin.cpp` 加一行注册；**不新增工具、不改工具 schema**。完整规范见 [扩展包开发手册](../../../../docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md)。

| 包 | 认领 | 底层 |
|:--|:--|:--|
| `memory` | `memory://out/N`、`memory://history`、`memory://cases/...` | `context` |
| `display` | `device://local/display/{text,qr,image,banner,clear}` | `ui_state` 浮层 |
| `hardware` | `device://local/led`、`device://local/audio/beep`、`sensor://imu/...` | `ops_hw` |
| `local` | 其余 `device://local/...`（状态、亮度、息屏、音量、剪贴板）与笔记类 `memory://...` | `ops_local` |
| `ssh` | `ssh://<主机>/...`：读写文件、执行命令、连接与断开 | `ops_ssh`（libssh） |
| `tmux` | `tmux://`、`tui://`、`agent://`：终端会话的观察与输入 | `ops_tui` |
| `agents` | `session` 的 `start_agent` / `resume_agent` / `interrupt` / `approve` / `reject`：远端 Codex、Pi、Claude | `ops_tui` |
| `policy` | `session` 的 `set_policy`：按 target 调整 deny / ask / allow | `policy` |
| `wifi` | `wifi://scan`、`wifi://status`、`wifi://<网络名>`（切换网络） | `ops_wifi` |
| `web` | `web://search`、`web://page` | `ops_web` |
| `lora` | `lora://...`（也接受 `device://lora/...` 别名） | `ops_lora`（RadioLib） |
| `gps` | `sensor://gps/...` | `ops_gps` |
| `files` | `file:///vat/...`（拒绝密钥与系统目录） | `storage` |
| `timer` | `timer://in/...`、`timer://at/...`、`timer://list`、`timer://cancel/...` | `ops_timer` |
| `grove` | `sensor://grove/...` | Ex_I2C 扫描 |
| `ir` | `ir://<电器名>/<动作>` | `ops_ir`（IRremote） |
| 命令目录（`catalog_package.cpp`） | 只供 `discover`：描述经过审核的远端命令（搜索词、风险、`--help`），执行仍走 `ssh` | `ops_ssh` |

`builtin.cpp` 里的注册顺序只在两个包可能认领同一调用时有意义（例如 `display`、`hardware` 要排在 `local` 前面）；认领范围应保持互不重叠。
