# E2E 验收

`tools/vat_e2e.py` 是单机版的自动验收。它运行在设备 SSH profile 指向的那台主机上，**只通过 Wi-Fi 调用设备自己的 Web API**，同时在主机上核对真实副作用（文件、目录、tmux 会话、命令输出）。设备上的全部工作——语音识别、模型调用、Policy、SSH、tmux——都由固件完成，测试脚本不参与任何一步。

```sh
# 设备已配置 tests/e2e_profiles.example.json 中的 profile（用户换成运行脚本的这台主机的用户）；
# 地址不用手改：每次开跑前，模板里写 <this-host-ip> 且设备上是局域网地址的测试 profile 会自动改成本机当前 IP
# （换了网络也能直接跑；公网地址的 profile 不动；--no-sync-profiles 跳过）
# 新扩展包必须在这里补用例，见 docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md §6
export VAT_DEVICE=http://<设备 IP>                                       # 不设时默认 http://vat.local
python3 tools/vat_e2e.py                                                  # 默认 auto：按改动挑阶段
python3 tools/vat_e2e.py --since v0.2          # auto 的比较基准（默认 HEAD~1，含未提交改动）
python3 tools/vat_e2e.py --phases s2 --only policy,approval   # 只跑指定用例
python3 tools/vat_e2e.py --phases quick        # 快速档：每次刷机后，≤ 5 分钟
python3 tools/vat_e2e.py --phases all          # 发布档：发版前，≤ 15 分钟
python3 tools/vat_e2e.py --phases full         # 完整档：设备上跑全部用例（旧的 all），约 25–30 分钟
```

## 跑多少

完整跑一遍设备上的全部用例（S0–S11 + S2I）约 25–30 分钟（v0.6 正常时 1384–1571 s），S1、S2I 是几十条云端语音 / 意图用例，占了大头。所以分四档：

| 档 | 跑什么 | 预计耗时 | 比完整档少了什么 |
|:--|:--|:--|:--|
| `quick` | 除 S2I 外每个阶段一两条代表用例：S1 两段录音；S3 两条命令；S4 不让 Agent 发 Ctrl+C / 看终端；S6 只有 discover、WiFi 扫描、大字浮层、一次 RGB 灯；S8 只有长输出；S10 五个程序（灯、分支、被调、调用、坏程序）加触发器 | ≤ 5 分钟（估算约 4.5 分钟，待实测） | 红外、空调选码、文件 / 定时器 / Grove、IMU、蜂鸣、WiFi 密码与序号指代、联网搜索、历史检索、案卷、压缩、多步 A/N 与密钥、20 轮耐久等 |
| `auto` / 阶段列表 | 改动影响到的阶段（下表），每个阶段完整跑；S1 只跑 10 段核心录音 | 视阶段 | S1 其余 18 段录音 |
| `all`（发布档） | 全部阶段，但：S1 只跑 10 段核心录音；S2I 设备上只抽 12 条（每类工具一条，`S2I_SAMPLE`），同时在主机后台用提示词实验台跑全部意图用例（结果记为 S2I 的 `prompt_lab_all_intents`，`--no-prompt-lab` 关掉）；S3 不跑 hostname / pwd / uptime / df 和连接 / 列表 / 断开；S5 耐久 10 轮；S7 查密钥泄漏只读本阶段新增或变了大小的文件；S8 延迟对比各 2 轮、案卷不单独重启（清空对话后取回） | ≤ 15 分钟（估算 14–15 分钟，待实测） | 18 段非核心录音的设备端识别（意图由实验台覆盖）；46 条 S2I 只在实验台上跑、没过设备拼的提示词；耐久轮数减半；案卷跨重启（S5 已测重启后对话恢复）；之前运行留下、本次没动过的文件不再查密钥 |
| `full`（完整档） | 设备上跑全部用例（v0.6 之前的 `all`） | 约 25–30 分钟 | — |

| 时机 | 跑什么 |
|:--|:--|
| 每次刷机后 | `quick` |
| 日常开发 | `auto`：S0 加上改动影响到的阶段（下表） |
| 修一条失败用例 | `--phases <阶段> --only <用例 id 片段>` |
| 改了 `agent_prompt.h` | 先用[提示词实验台](#提示词实验台)调到不退步，再跑 S2、S2I、S6、S7、S8 |
| 发版打标签前 | `all` 跑一次，结果写进 [证据](evidence/README.md)；改了语音链路、提示词或做了大改动的版本跑 `full` |

所有档都不再用固定等待：重启后、S10 一次性触发器、S10 运行记录、S11 面板都改成带超时的轮询（超时与原来的等待时长相同）；S11 的本地应用仍固定等 13 秒，因为这条用例要看应用撑过一次 10 秒刷新。S4 的 Pi 没启动起来时直接判失败，不再空等约 100 秒。S7 的一次性 SSH 主机跑完不删，下次复用（只删掉设备上次装的公钥，登录仍要密码）。

改动与阶段的对应关系（脚本里的 `IMPACT`，按最长前缀匹配；S0 总会跑）：

| 改动 | 阶段 |
|:--|:--|
| `agent.*` | S2 S2I S7 S8 |
| `agent_prompt.h` | S2 S2I S6 S7 S8 |
| `stt.*`、`tests/pcm/` | S1 |
| `ui_recorder.*`（录音） | S1 S4 |
| `net.*` | S1 S3 S8 |
| `ops_ssh.*` | S3 S7 S8 |
| `packages/ssh_package` | S3 S7 |
| `context.*` | S2 S5 S8 |
| `ops_web.*`、`packages/web_package`、`packages/memory_package` | S8 |
| `ops_tui.*`、`packages/tmux_package`、`ui*`（含 `ui_kit`） | S4 |
| `worker.*` | S4 S7 |
| `ops_local.*`、`policy.*` | S2 |
| `common.*` | S2 S4 |
| `tests/cases/` | S2I |
| `registry.*`、`ops_wifi.*`、`ops_hw.*`、`ops_timer.*`、`ops_ir.*`、其他 `packages/*` | S6 |
| `storage.*` | S5 S8 S9 |
| `config.*` | S5 S9 |
| `standalone_agent.ino` | S1 S5 |
| `web.*`（Web API） | S7 |
| `ops_lora.*`、`ops_gps.*`、`packages/lora_package`、`packages/gps_package` | S9 |
| `tests/e2e_profiles.example.json` | S3 |
| `settings_*`、`ops_portal.*`、`web_portal.*`、文档 | 只跑 S0（设置页用例都在 S0） |

`--only` 只过滤 S1/S2/S2I 的用例矩阵（PCM、多轮、意图）；S1 被过滤时跳过延迟统计，S2 的审批场景要显式加 `approval` 才跑。新加的源文件要在 `IMPACT` 里登记。

结果写入 `logs/standalone-e2e-<时间>.json`（含每条用例的调用、决策、耗时，以及设备运行日志尾部）。

## 各阶段覆盖

| 阶段 | 验收内容 |
|:--|:--|
| S0 启动与配置 | 存储挂载、配置完整、Wi-Fi 联网、不在 Setup、READY 页、配置页、文件 API 读写；E2E profile 齐全；**主机上没有任何 gateway 进程，USB 串口无人占用**。设置页：菜单与 WiFi 列表导航、WiFi 扫描、增删测试主机、设备公钥页、过短的 Key 被拒绝；手机设置：热点开启与关闭、提示正确、状态接口里不出现 Key 和热点密码 |
| S1 语音链路 | （`full`）28 段 PCM（5 段本机设置 + 23 段意图）上传到设备，设备自行完成识别 → Agent → 执行；检查转写关键词、工具与参数；P50/P95 延迟；TEXT 模式只转写不执行。其他档只跑 `CORE_PCM` 的 10 段（`quick` 2 段） |
| S2 运行时 | 9 条多轮对话（期望值里的 `{user}`、`{hostname}` 取自运行脚本的主机）；真实审批：批准才执行、拒绝不执行、批准写文件、凭据路径批准也拒绝、策略切到 ask 后生效再切回 |
| S2I 意图矩阵 | 41 条 SSH 意图 + 17 条日常意图，每条独立上下文，判定规则：工具名顺序相等、期望参数逐键相等。v0.5 新包的 15 条意图（`v05_intent_cases.json`）目前只在实验台上跑。`all` 档设备上只跑 `S2I_SAMPLE` 的 12 条，其余交给后台的实验台 |
| S3 SSH | `whoami/hostname/pwd/uname/uptime/df -h` 与主机实际输出比对；读 `/etc/hostname`；discover 探测 Docker；`docker ps`；连接/列表/断开；带工作目录的 `git status`。`all` 档只跑 whoami、uname、读 `/etc/hostname`、Docker 两条和 `git status` |
| S4 TUI 与界面 | 语音“连接开发机”后 SSH 页接入 `vat-shell`；会话被结束后页面提示重新连接；时间线光标与详情、帮助翻页、截图接口；创建 tmux 会话；终端页输入并观察回显；Agent 发送 Ctrl+C、观察终端；SSH 页内容、切页；Pi 只读启动、作答、中断（远程 Agent 链路与 provider 无关，测试机上只有 Pi 处于登录状态）；Esc 取消进行中的请求且设备随后可用 |
| S5 恢复与耐久 | 重启后联网就绪、对话记忆与时间线恢复、重启后仍记得上下文、tmux 会话可直接观察；连续 20 轮（`all` 档 10 轮）无失败无重启；最低可用堆 ≥ 60 KB；再次确认无 gateway、无 USB 数据连接 |
| S6 能力包 | discover 列出全部已注册包；语音“搜索附近 WiFi”路由到 `device://local/wifi/scan` 且结果含当前网络；查询当前网络；语音“连接 WiFi X，密码是 …”时设备弹出密码输入框（说出的密码不被使用），放弃后配置和连接都不变；未注册的 scheme 不被猜测执行；v0.5 本机扩展包：大字浮层、RGB 灯与提示音免确认、IMU 姿态、充电字段、`file://` 列目录与拒绝读取密钥、`sensor://grove` 扫描、10 秒提醒真实触发、红外“发码 → 没反应 → 换码 → 生效”流程；**序号指代**：搜索 WiFi 后“连接第 N 个”弹出的正是第 N 个网络的密码框，列出 tmux 会话后“接入第 N 个”，列出容器后“看第 1 个的日志” |
| S7 多步任务与密钥 | 依赖上一步结果的多步只读任务；逐步确认（每步 Y）、A 全部批准、N 拒绝后不执行；在一次性 Docker SSH 主机（`linuxserver/openssh-server`，2222 端口）上：输入登录密码后装好公钥、之后免密；sudo 正确密码得到 `root`、错误密码明确失败；密钥不出现在任何存储文件（`full` 遍历 `/vat` 下全部文件完整读取；其他档只读输入密码之后新增或大小变了的文件）；终端出现密码提示时切换为掩码输入 |
| S8 长上下文与联网 | 需要 TF 卡。主机生成 310 KB 日志，问“第 4 个 ERROR 的错误码”：答对，且用 `memory://out/list` 取而不是重跑；完整输出存满 256 KB 并写明截断；清空对话后从归档检索答出；案卷记录、重启后自动带入（`all` 档不重启，清空对话后带入）、关闭；40 KB 窗口下单轮中位数（`all` 档各 2 轮）不超过空窗口的 2 倍；窗口超预算后压缩到 48 KB 内、摘要生成、旧事实仍可回答；联网搜索与读网页；全程最低可用堆 ≥ 40 KB |
| S9 外设 | 需要 Cap LoRa-1262。GPS：模块有 NMEA 输出（室内不要求定位）。LoRa：状态可读；频点超出 868–923 MHz 被拒绝；未设频点时拒绝开启；未确认天线时拒绝发送；关闭后状态正确。不设置频点、不确认天线、不发射 |

需要人在实物上确认的项目（按键手感、真实录音、扬声器、拔掉 USB 用电池运行）见 [人工验收](MANUAL_ACCEPTANCE.zh-CN.md)。

## 提示词实验台

改提示词不必每次上实机。`tools/prompt_lab.py` 把设备发出的请求原样发给模型 API：系统提示词和工具定义直接从 `agent_prompt.h` 抽取，模型名读设备配置，SSH 主机名读设备状态（v0.6 起设备接口要令牌，实验台自己读不到时用 `--model`、`--profiles` 传入；发布档里由 `vat_e2e.py` 读好后传给它，实验台离线跑），`discover` 用缓存的设备真实回答；遇到第一个会被设备执行的操作就停下，用和 E2E 相同的 `validate_expect` 判定。所有意图用例（S2I 的两组 + `v05_intent_cases.json` + PCM 清单里的期望）并发跑，约 2 分钟。

```sh
export VAT_DEVICE=http://<设备 IP>          # 读取模型名、主机名；不设则离线，只用缓存
python3 tools/prompt_lab.py                  # 当前提示词
python3 tools/prompt_lab.py --prompt variant.txt          # 候选提示词（纯文本）
python3 tools/prompt_lab.py --only led,timer --repeat 3   # 只跑部分用例，每条 3 次看稳定性
```

discover 的回答缓存在 `tests/fixtures/discover_cache.json`（入库）；未命中时经设备的 worker 任务查询（`POST /api/job kind=discover`，与模型拿到的结果逐字相同，含 2000 字节截断），每个用例最多 2 次 discover，与设备一致。包的说明改动后用 `--refresh` 让设备重新回答全部缓存。

Key 从仓库外的 `../.deepseek_key` 读取，不会打印。实验台通过后仍要按上表在实机上跑受影响阶段。

## 语音用例

`tests/pcm/` 里的 28 段音频全部由 TTS 合成（不是人声录音），参数记在 `generated.json`。新增语音用例：在 `manifest.json` 加一条（原文、转写关键词、期望调用），然后运行 `tools/gen_pcm.py` 合成缺少的文件（需要 OpenRouter Key 和 ffmpeg）。

## 最新结果

见 [证据](evidence/README.md)。
