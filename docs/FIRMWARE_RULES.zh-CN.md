# 固件开发约束

Cardputer Adv 没有 PSRAM，只有约 320 KB 内部 RAM。下面每一条都来自 V1 开发中实际出现过的故障，违反任何一条都可能在长时间使用后出问题，而且往往要跑几十轮才会暴露。PR 评审按此逐条检查。

## 1. 内存

| 规则 | 原因（真实故障） |
|:--|:--|
| 不要在 RAM 里拼接大请求体或整段音频。模型请求用 `net::post_segments` 把 Flash 常量和小片段直接流进 TLS；语音用 `post_pcm_base64` 边读边编码 | 曾因拼接 30 KB 请求体加 cJSON 树，导致堆峰值跌到 5 KB |
| 系统提示词和工具 schema 以转义好的 JSON 常量放在 Flash（`agent_prompt.h`），不要在运行时复制 | 同上 |
| 长期存活的数据（对话历史、任务记录、日志）放存储，按需读取，不常驻内存 | 常驻的 String 把堆切碎，最大连续块从 188 KB 掉到 31 KB |
| TLS 连接和 SSH 会话依次使用 `net::release_reserve()` / `restore_reserve()` 预留的大块内存；两者**绝不同时存在**：请求模型前 `ssh::release()` | 碎片化后 mbedTLS 报 `-32512 Memory allocation failed`，之后每个请求都失败，直到重启 |
| 远端命令输出在远端限长（`ssh::run` 的 `max_output`，默认 4 KB），不要调大到 16 KB 以上 | libssh 每个包都要一块连续内存，大输出时分配失败会把 `stdout_buffer` 置空后再解引用，直接 panic |
| 大文件（窗口、输出、网页、日志）不读成 String：发请求用文件片段，收响应用 `post_json_to_file`，Web API 分块发送，长输出用分页 | Arduino `String` 分配失败时静默变成空串：`/api/file?tail=60000` 曾一直返回空，让 E2E 的密钥扫描假通过 |
| SSH 长输出不调大单次读取上限，用 `run_keeping` + `fetch_output` 每次取 12 KB | 见上一条 libssh 规则 |
| cJSON 在内存不足时会**静默**失败（返回空、字段缺失）。凡是要落盘或发给模型的结构，写入前和读回后都要校验（参考 `agent.cpp` 的 `valid_turn`） | 残缺的历史记录落盘后，之后每次请求都返回 422 |
| worker 任务栈 32 KB（实测峰值约 15.7 KB）。新增深递归或大的栈上缓冲区前，先看 `/api/status` 里的 `worker_stack_free_min` | 栈溢出会悄悄覆盖相邻的堆对象 |

## 2. 音频与存储

| 规则 | 原因 |
|:--|:--|
| 麦克风只在录音期间开启（`Mic.begin()` → 采集 → `Mic.end()`），之后才能做任何文件写入 | 麦克风采集期间写板载 Flash，会让 M5Unified 麦克风永久失效，`end/begin` 都救不回来，只能重启 |
| 只有板载 LittleFS 时，录音以 μ-law 暂存在内存（约 5 s），停麦之后再写文件；有 TF 卡时可以直接写卡 | 同上；TF 卡走 SPI，不会暂停 Flash 缓存 |
| 扬声器保持关闭（`internal_spk=false`） | 与麦克风共用 I2S，切换会破坏录音 |
| LittleFS 下日志按 48 KB 轮转；新增的持久文件必须有上限或轮转 | 1.5 MB 的 LittleFS 曾被写满，导致录音失败 |
| 同一个文件可能被 UI 任务和 worker 同时追加，统一走 `storage::append_line`（带锁） | 曾出现两行日志交错写成一行坏 JSON |
| TF 卡与 Cap LoRa-1262 共用一条 SPI 总线：挂载存储前拉高 LoRa 片选 G5；LoRa 驱动与存储共用同一个 `SPIClass`，每次访问前拿总线锁 | G5 悬空时 TF 卡读不出类型（`type=0`），设备退回板载存储 |
| 开机必需的东西（配置、密钥、主机、设备私钥、笔记、设备档案）在 Flash `/mirror/` 有镜像；其余只在卡上 | 卡坏时设备曾进入初次配置模式完全离线，私钥也只在卡上 |
| 数据只存一处：插卡时把板载 `/vat` 搬到卡上并清空板载副本，不在两处各留一份 | 曾读回卡上一天前的旧配置，缺两台主机 |
| TF 卡跑 10 MHz（不行再 4 MHz），不要回到 25 MHz；屏幕在 M5GFX 里是 SPI3、TF 卡是 SPI2，**不要把卡挪到 SPI3** | 插上 Cap 后 SPI 线变长且分叉，25 MHz 下卡间歇性不应答；SD 库每次读扇区原地忙等 500 ms×3，查一次目录超过 5 s，看门狗重启（串口回溯止于 `sdReadBytes → spiTransferByteNL`），读录音上传时也失败。曾误以为卡和屏幕共用 SPI2 而把卡挪到 SPI3，结果正好和屏幕撞上、主循环卡死；也试过放宽看门狗、把工作线程挪到 CPU 1，都只是掩盖或另生问题 |
| 测试时用 `make serial-log` 记录串口；看门狗与 panic 的现场只打印在串口上 | 第一次看门狗重启的现场因此丢失 |
| TF 卡只开 3 个文件槽（`kSdMaxFiles`），遍历目录先列出子项再递归，不在递归时开着父目录 | FATFS 挂载时按槽预分配约 4 KB/槽；5 个槽让空闲堆少 20 KB，TLS 握手时最低堆跌到 25 KB |

## 3. 密钥

| 规则 | 原因 |
|:--|:--|
| 密钥只从设备输入框（或测试用的 `/api/secret`）进入，经 `execute_secret()` 传给执行它的那一步 | 语音会经过云端识别，模型参数会进入上下文和日志 |
| 远端命令的密码走 stdin（如 `sudo -S -p ''`），不拼进命令行 | 命令行会出现在进程列表和日志里 |
| 一次性密码登录不缓存会话，用完立即 `close_cached()` | 避免拿着密码认证的会话继续被别的操作复用 |
| E2E 必须检查密钥没有出现在任何存储文件里 | 回归防护 |

## 4. 界面

| 规则 | 原因 |
|:--|:--|
| 不使用全屏离屏缓冲（240×135×16 位 = 64 KB） | 内存余量承受不起；直接绘制，动画刷新 ≥ 400 ms |
| 例外：右下角 32×32 吉祥物按素材帧时长刷新（≥ 100 ms） | 整帧先读进 1 KB 静态缓冲（调色板索引），一次推送、不先清再画，所以不闪；页面重绘永远不画到这块 |
| 显示事务（`startWrite` … `endWrite`）里不读写文件 | 事务期间屏幕占着总线；先读再画（吉祥物 `load()` 在事务外、`push()` 在事务内） |
| 字库里不确定存在的符号（`● ◐ ○ • ▌ ⇄ ‹` 等）用图形绘制或换成 ASCII | efontCN 缺字会显示成方框；用 `/api/screenshot` 截图确认 |
| 配色只用主题里的 7 个颜色 | 见 `ui.cpp` 顶部的 theme 常量 |

## 5. 任务与线程

- 网络、SSH、模型调用只能在 worker 任务里执行；UI 任务（Arduino `loop`）只负责绘制、按键、录音和 Web 接口，不能阻塞等待网络。
- 屏幕只由 UI 任务绘制；worker 通过 `ui_state` 修改界面模型。
- 可能阻塞的读写都要登记到取消令牌（`cancel::set_socket`），让 Esc 能立即中断。**但凡是会经过 Arduino `Stream::timedRead()` 的读取（如 `HTTPClient::getString()`）必须先注销**：它是不让出 CPU 的忙等循环，socket 被关闭后会空转到超时，触发任务看门狗重启（曾在 Esc 取消时复现）。
- Web 接口在空闲堆低于 40 KB 时只返回精简结果或 503（`web.cpp` 的 `low_memory`）；新增的较重接口也要加这个保护。

## 6. 网络与远端

- 远端命令只能由 argv 组装（`shell_join`），写文件的内容走 stdin。
- 每个 HTTP/SSH 操作都要有超时；HTTP 对传输错误和 5xx 自动重试一次（取消时不重试）。
- 设备新建的 tmux 会话使用极简 shell（`PS1='$ '`，47×11），适配 240×135 屏幕。

## 7. 构建与发布

- 往构建机同步源码时**不要保留时间戳**（`rsync --no-times`），或者先清空 `build/`。保留本地时间戳曾导致增量编译跳过修改，刷进设备的是旧代码。
- 刷写后确认版本确实更新了：看 `/api/status` 的 `uptime_s` 和 `reset_reason`，必要时用 `strings build/…/*.elf | grep <新字符串>`。
- 崩溃时串口会输出回溯，用 `xtensa-esp32s3-elf-addr2line -pfiaC -e build/standalone_agent/standalone_agent.ino.elf <地址…>` 解码；`/api/status` 的 `reset_reason` 会显示 `panic` / `task_wdt` 等原因。

## 8. 诊断接口

| 接口 | 用途 |
|:--|:--|
| `/api/status` | `free_heap` `max_block` `min_free_heap` `open_sockets` `worker_stack_free_min` `reset_reason` |
| `/api/action?name=mic_test` | 不写文件地采集 0.3 s，检查麦克风 |
| `/api/action?name=record_test` | 完整走一遍“按住 Enter 2 秒”的录音流程 |
| `/api/screenshot` | 当前屏幕的 BMP 截图，用来远程检查界面 |
| `/vat/logs/runtime.jsonl` | 每个阶段的耗时、HTTP 状态、可用堆、最大连续块 |
