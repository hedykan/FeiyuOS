# 扩展性审计（U4）

> 2026-11-12，分支 `feat/v0.7.3`（v0.7.2 发布后）。依据仓库根 `CLAUDE.md` 的四条约束：两态模型、禁止补丁式开发、扩展包 = 资源地址 + 可选 Lua 函数、模型只有七个工具。逐项回答 `docs/V072_PLAN.zh-CN.md` 里 U4 表格的五个问题。代码位置写成「文件:行」，文件都在 `firmware/standalone_agent/src/` 下。
>
> 结论分三档：**本版改**（0.7.3，小而安全）、**下版改**（写明版本：0.7.4 提示词瘦身、0.7.5 屏幕绘制统一、0.7.6 程序共用数据、0.8 独占态）、**合格，记录即可**。

## 1. 页面：四个页面的绘制和按键

页面是 `ui_state.h:14` 的 `enum class Page { Agent, Terminal, Program, Graphics }`，另有 `View`（列表 / 详情 / 帮助）叠在页面上。

### 绘制

`render()`（`ui.cpp:953`）按页面分成三条互不相干的路：

| 页面 | 走哪条路 | 清屏怎么判断 |
|:--|:--|:--|
| Graphics | `overlay::draw()`（`ui.cpp:959`），整屏交给浮层 | 浮层自己 `fillScreen`（`ui_overlay.cpp:333`） |
| Program（列表视图） | `draw_program_page()`（`ui.cpp:822`），自己开显示事务、自己画顶栏 | `program_layout`（`ui.cpp:67`、`:836-840`） |
| Agent、Terminal、帮助、详情 | `paint` 闭包（`ui.cpp:1023-1040`） | `drawn_layout`（`ui.cpp:1020-1022`），Terminal 又单独按行清（`ui.cpp:639` 起） |

- 「布局变了就清屏」有两套各自的状态（`program_layout` 与 `drawn_layout`），v0.7.2 的 U1（Agent 页底栏残留在程序页）就出在这两套之间。**下版改（0.7.5）**：一个视图表、一个渲染循环、一处清屏判断。
- 列表的绘制基本是共用的：程序列表（`ui.cpp:763-766`）、帮助（`ui.cpp:617-619`）、设置页（`settings_page.cpp:185-188`）都走 `CursorSlide` + `scrolled_rows`（`ui_kit.h`）。Agent 时间线（`ui.cpp:330`）因为一条可以占多行，用两个 `CursorSlide` 自己画；面板的选项（`ui_overlay.cpp:41`）用 `band` 自己画。**合格，记录即可**；0.7.5 收成一个「列表」组件时把这两处也并进去。
- 底栏提示有四套：`program_footer`（`ui.cpp:740`）、设置页 `footer`（`settings_page.cpp:156`）、时间线底栏（`ui.cpp:450`）、浮层里的 `drawRightString("任意键返回")`（`ui_overlay.cpp:365`、`:377`）。**下版改（0.7.5）**：一个底栏组件。
- 文字折行有两套：`wrap()`（`ui.cpp:99`，带行数上限和省略号，给时间线、终端用）和 `wrap_text()`（`ui_kit.cpp:142`，给控制台、浮层、面板用）。两者都是逐字符拼 `String`、反复 `substring`，每画一次都在 UI 任务上产生大量小块分配，和 0.7.3 的内存碎片问题有关，但改它会动所有页面的排版。**下版改（0.7.5）**：合成一个按字节偏移切行、不拼中间串的实现；0.7.3 如果量到 UI 任务的分配是碎片来源之一，再提前。

### 按键

`dispatch_keys()`（`ui.cpp:1186-1389`）一个函数管所有页面：

- Graphics 在最前面单独处理（`ui.cpp:1193-1211`）；设置页、密钥输入、帮助、Esc 依次截走；Fn 组合键里每一条都带 `current_page == X` 条件（`ui.cpp:1247-1279`）；Program 交给 `program_key()`（`ui.cpp:880`）；Agent 写在函数里（`ui.cpp:1322-1360`）；剩下的都当 Terminal（`ui.cpp:1362-1389`）。
- 同一个键在不同页面意义不同（`;` `.` 在 Agent 页移动光标，在 Terminal 页要按 Fn 才发方向键），这是对的，但规则散在 if 链里，新增一个页面要改这个函数的好几处。
- 页面切换 `switch_page()`（`ui.cpp:1076-1098`）里有每个页面的进入逻辑（Graphics 记上一页、Program 判断列表是否过期、Terminal 刷新终端）；`next_page()`（`ui.cpp:1099-1105`）把循环顺序写死。
- 网页接口的屏幕模型 `screen_text()`（`ui.cpp:1530-1589`）按页面拼标题和正文。

**下版改（0.7.5）**：页面成为数据（`{名字, 绘制, 按键, 进入, 屏幕模型}`），`dispatch_keys` 只做全局键（Esc、Fn+H、Fn+W、Fn+A），其余交给当前页面；这正是 U5 的设计，审计结果作为 U5 设计文档的输入。

### 和两态模型的关系

- Terminal 页固定绑定 `tui://current`（`worker.cpp:181`、`:379`），是「一个资源一个专门页面」。在 OS 态里可以接受，**合格，记录即可**。
- Graphics 页是程序运行时的整屏画面，现在作为 OS 态里的浮层存在，系统界面和程序画面靠 `previous_page`、`input_from_graphics`（`ui.cpp:43-44`）、`apply_hardware_requests()`（`ui.cpp:1405-1430`）来回切。按两态模型，整屏画面属于独占态。**下版改（0.8）**；0.7.5 统一绘制时先保证程序的画法和系统页面是同一套，0.8 再把整屏交给独占态。

## 2. 屏幕上的东西：能不能收成一套绘制底层 + 几种容器

| 东西 | 谁写入 | 谁画 | 画法 |
|:--|:--|:--|:--|
| 全屏大字 / 二维码 / 图片 | `display` 包（`device://local/display/{text,qr,image}`），只构造 `Overlay` 结构（`packages/display_package.cpp:56`、`:104`） | `ui_overlay.cpp` 的 `draw_overlay()`（`:330`）三个分支 | 直接调 M5GFX |
| 面板 | `ui` 包 `ui://panel`、`ui://apps`（`packages/ui_package.cpp:81`、`:129`） | `draw_panel()`（`ui_overlay.cpp:61`）、`draw_choices()`（`:41`） | 直接调 M5GFX |
| 画布 | `ui` 包 `ui://canvas`（`packages/ui_package.cpp:51`） | `draw_canvas_on()`（`ui_overlay.cpp:192-318`）按指令表逐条执行 | 白名单指令，和 M5GFX 同名同参 |
| 程序控制台 | `console` 模块（程序的 `print`、`io.read`） | `draw_console()`（`ui.cpp:786`） | 直接调 M5GFX |
| 横幅（一行通知） | `display` 包 `banner` | 时间线底栏 | 直接调 M5GFX |

- 扩展包这一侧是干净的：`display`、`ui` 两个包都只构造数据交给界面，自己不画（`ui_canvas.cpp`、`ui_panel.cpp` 只做解析和校验，能在主机上测）。**合格，记录即可**。
- 画的这一侧，五种东西是 `draw_overlay()` 里的五个分支加 `ui.cpp` 里的控制台，各自直接调 M5GFX，各自画「任意键返回」。
- **事实上的绘制底层已经存在**：画布的指令表（`ui_canvas.h` 的说明、`ui_overlay.cpp:192` 的解释器）就是「和 M5GFX 同名同参、有白名单和上限」的一套原语，模型也已经会用。0.7.5 的 `gfx` 建议就从它出发，而不是另起一套：
  - 绘制原语 = 画布指令表（再补 `measure`、屏幕尺寸），对外是 `gfx://` 资源地址，对程序是 `gfx.*` 函数，两边一一对应（P3）。
  - 大字、二维码、图片、面板、横幅 = 用这些原语预先写好的几种画法（容器），不再各有分支。
  - 容器的位置只有三种：整屏（Graphics 页 / 独占态）、页面正文区（程序控制台、面板）、顶栏或底栏（状态、横幅）。
  - `display` 包对外的地址不变，只是内部改成调同一套原语。
- 结论：**下版改（0.7.5）**。本版不动。

## 3. 资源地址命名

### 各扩展包认领的地址（`supports()`）

| scheme | 扩展包 |
|:--|:--|
| `device://local/`（除下面三类） | local（`packages/local_package.cpp`） |
| `device://local/display/{text,qr,image,banner,clear}`（及别名） | display（`packages/display_package.cpp:24-30`） |
| `device://local/{led,light,rgb}`、`device://local/audio/{beep,tone}`、`device://local/beep` | hardware（`ops_hw.cpp`） |
| `sensor://imu` | hardware |
| `sensor://gps` | gps |
| `sensor://grove` | grove |
| `memory://out`、`memory://history`、`memory://cases` | memory（`context.cpp:214`） |
| `memory://notes`、`memory://recent-tasks` | local |
| `ssh://` | ssh；`session set_policy` 由 policy 包认领 |
| `tmux://`、`tui://`、`agent://`（read / observe / input） | tmux（`packages/tmux_package.cpp:12`、`:31`） |
| `agent://current`（session 的 start / resume / interrupt / approve） | agents |
| `wifi://`、`ble://`、`lora://`、`ir://` | wifi、ble、lora、ir |
| `http://`、`https://` | http |
| `web://` | web |
| `place://` | place |
| `file://` | files |
| `timer://` | timer |
| `prog://`、`trigger://` | program |
| `pkg://` | pkg |
| `ui://` | ui |
| `lua://run` | lua |
| `settings://` | settings |
| `ask://` | ask |
| `debug://` | debug |
| 卡上扩展包自己声明的 scheme | card（`packages/card_package.cpp:51`） |
| 事件：`time://…`、`device://boot`、`ui://panel/名字`、`ble://event/…`、`prog://event/failed` | timer、local、ui、ble、program |

### 同类东西放在两处、命名不一致

1. **本机硬件分在三个前缀、四个包**：灯、提示音是 `device://local/*`（hardware 包），屏幕显示是 `device://local/display/*`（display 包），屏幕亮度又是 `device://local/display/brightness`（local 包），姿态是 `sensor://imu`（hardware 包）。同一个前缀 `device://local/display/` 被两个包按第二段切开（`packages/display_package.cpp:24-30` 用排除法让出 brightness / timeout / power）。
2. **前缀重叠靠注册顺序解决**：local 认领全部 `device://local/*` 和 `memory://*`（`packages/local_package.cpp` 的 `supports()`），display、hardware、memory 必须注册在它前面（`packages/builtin.cpp:37-40` 的注释）。调换顺序就会静默地换归属。**下版改（0.7.4）**：local 改成明确列出自己认领的地址，各包的认领互不重叠，注册顺序不再影响结果。
3. **设置分在四处**：`settings://`（设置页各项）、`device://local/display/{brightness,timeout}` 与 `device://local/audio/volume` 与 `device://local/context/length`（local 包）、`lora://settings/*`、`ble://power`。lora 的用法里还要专门说「用 lora:// 而不是 device://」。
4. **位置有两个入口**：`sensor://gps/location`（gps 包）和 `place://here`（place 包），两个包的用法文本都写了「用户问我在哪」，一个说用 GPS、一个说优先 place（见第 4 节）。
5. **定时有三样东西、两种写法**：`timer://in/10m`（提醒，到点显示一句话）、`trigger://new` 加事件 `time://in/10m`（到点运行程序）、`vat.after("10m", …)`（就是建一个 `time://in` 触发）。`timer://in` 与 `time://in` 只差一个字母、意思不同，timer 包的用法要专门提醒「到点要执行动作不要用 timer」。
6. **`memory://` 分给两个包**：out / history / cases 归 memory 包，notes / recent-tasks 归 local 包。
7. **`agent://` 分给两个包**：读、看、输入归 tmux 包，会话动作归 agents 包。
8. **别名表**：display 包接受 `qrcode`、`notification`、`toast` 等猜出来的名字（`packages/display_package.cpp:31` 起），hardware 包接受 `light`、`rgb`、`tone`。这是为模型猜名字打的补丁；有了 `discover` 应该不需要。

**结论**：第 2 条 **下版改（0.7.4）**。第 1、3、4、5、6、7、8 条改名会影响卡上已有的程序和模型的习惯，**下版改（0.7.4）**：提示词瘦身时一起收拢（每一类只留一个入口，旧地址保留为别名并在 `discover` 里只列新地址），先用实验台量别名的命中率再决定删不删。第 5 条要不要把提醒也做成「触发 + 一个显示通知的内置动作」，放到 0.7.4 设计时定。本版不改地址。

## 4. 提示词：协议 / 约定，还是场景特例

每次请求的系统提示词约 24 KB（kCorePrompt 3578 字节、kClosingPrompt 1783 字节、各包用法合计 18592 字节），实验台量到约 7000 个输入 token。

### kCorePrompt（`agent_prompt.h:12`，按句编号）

| 句子 | 内容 | 判断 |
|:--|:--|:--|
| C1–C9 | 身份、七个工具、何时调用、先 discover、不猜不完整 target、write 内容逐字 | 协议 |
| C10 | write 的 mode（设备状态用 overwrite，文件按说法） | 协议，但例子里的「门锁、GPIO」是特例 |
| C11 | confirm_required 的规则 | 协议；和 Policy 的实际判断重复（设备会自己决定是否确认，见 C19） |
| **C12、C13、C15** | 传感器、灯、门锁、插座、继电器、ESP GPIO 的地址写法，「书桌温度传感器是 sensor://书桌/temperature」「厨房 ESP 的 2 号 GPIO 是 device://厨房ESP/gpio/2」 | **场景特例**。设备上没有任何扩展包认领 `sensor://书桌` 或 `device://厨房ESP`，这些句子是早期意图评测的答案写进了提示词 |
| **C14、C17** | 没说名称必须澄清、不要用 discover 猜目标 | **场景特例**，并且和 kClosingPrompt 第五条「先 discover，不要先反问」相反；第五条只能靠一句「这一条优先于前面先澄清名称的说法」压过它 |
| **C16** | 车库不能翻译成 garage | **场景特例** |
| C18 | 危险请求没有明确目标必须澄清 | 约定 |
| C19、C21、C25 | 确认由设备决定、密钥只在设备上输入、不假装执行 | 协议 |
| C20、C22 | 回复简短、多步任务的总结 | 约定（和 kClosingPrompt 第三条有重叠） |
| C23、C24 | 编号清单与「第 N 个」 | 约定 |

### kClosingPrompt（`agent_prompt.h:16`）

五条（改完确认、失败先读原因、分开说已确认和未确认、有歧义用默认值、先 discover 再问）都是协议 / 约定。第四条里的两个默认值（写程序 = 本机 Lua、灯 = 本机 RGB 灯）是约定，可以保留。第五条那句「优先于前面」是在给 C14、C17 打补丁，删掉 C14、C17 后它也应去掉。

### 各包用法（`registry::usages()`，按注册顺序）

| 包 | 字节 | 判断 |
|:--|:--|:--|
| memory | 664 | 协议 |
| display | 141 | 协议 |
| hardware | 117 | 协议 |
| local | 1118 | 地址清单是协议；剪贴板、记忆笔记各带一个评测原句的例子（「把部署完成复制到剪贴板」「记住明天检查服务器证书」），是**场景特例** |
| settings | 677 | 协议；状态栏可选项列的是「时间、电量、信号、WiFi名、内存、蓝牙」，0.7.3 新加的「后台失败」要补进去 |
| ssh | 2186 | 地址格式、exec 拆 program / args 是协议；**场景特例**：「NGX、N G X 指 nginx」「API 容器」「docker logs 的选项放在容器名之前，最后 50 行必须是 …」「删除目录使用 rm -rf」「ETC/hostname 规范成 /etc/hostname」、繁体 profile 转简体 |
| tmux | 958 | target 规则是协议；「XTheTMUX绘画、XTMX绘画」这类语音识别纠错是**场景特例** |
| agents | 774 | 动作是协议；「Voice Agent Terminal 规范为 voice-agent-terminal」「Backend 必须是 backend」「扣的差通常指 Codex」是**场景特例** |
| policy | 187 | 协议 |
| wifi | 329 | 协议（「家里 WiFi」是格式示例） |
| web | 503 | 协议 |
| http | 478 | 协议 |
| lora | 270 | 协议；开头「用 lora:// 而不是 device://」是在弥补第 3 节的命名不一致 |
| gps | 187 | 和 place **互相矛盾**：都写「用户问我在哪」，入口不同 |
| ble | 675 | 协议 |
| place | 632 | 见 gps |
| files | 128 | 协议 |
| timer | 311 | 协议；后半句在弥补第 3 节第 5 条的命名陷阱 |
| grove | 47 | 协议 |
| ir | 849 | 协议（空调指令格式较长，但是格式不是场景） |
| pkg | 533 | 协议 |
| ui | 2725 | 协议，但把画布的全部指令名、参数、动画写法都放进了每次请求；这些应该在 `read ui://help` 里按需读（像 `prog://help` 一样） |
| program | 3564 | 协议，0.7.2 实验台验证过；其中写法细节（返回表的字段、补丁格式）可以挪到 `prog://help`，用法里只留「什么时候用、去哪里看」 |
| debug | 539 | 协议 |

**结论**：

- 场景特例（C12–C17、ssh / tmux / agents / local 里的评测原句和语音纠错）、gps 与 place 的矛盾、ui 与 program 的长篇写法：**下版改（0.7.4）**。删场景特例前要先跑意图回归（`tools/prompt_lab.py`）：有几条意图用例（例如 `read_humidity` 期望 `sensor://书桌/humidity`）本身就是按这些特例写的期望，要一起改成「设备上没有就 discover 后如实说」。语音纠错如果确实需要，应做成数据（一张纠错表，在语音识别之后、进模型之前替换），而不是提示词。
- settings 用法补上「后台失败」：**本版改**（一句话，和 0.7.3 新加的状态栏项一起提交）。

## 5. 工具分发：按工具名、按地址前缀写死的特判

| 位置 | 内容 | 判断 |
|:--|:--|:--|
| `registry.cpp`（`execute()`，约 `:480`） | 第一个 `supports()` 认领的包执行，没人认领就返回「没有能力包可处理」 | 合格 |
| `agent.cpp:183-192` | 确认框的摘要按工具名（exec / write / session）组织 | 合格：七个工具本身就是协议 |
| `agent.cpp:555-560`、`:650` | discover 不计步数、结果不归档 | 合格：discover 是七个工具之一，交给 `registry::discover` |
| **`agent.cpp:440`、`lua_vm.cpp:603`、`agent.cpp:548-550`** | 「失败」靠结果文字的前缀判断，三处各写一份：`failed_result()` 认 `ERROR`、`没有能力包可处理`、`已拒绝`；`lua_vm` 认前两个；「同一调用连续失败」（`agent.cpp:548-550`）只认 `ERROR` | **本版改**：一个 `registry::failed(text)`，三处都用它。这正是 0.7.3「结果统一带成败状态」的第一步；以后扩展包直接返回状态而不是靠前缀，可以在 0.7.4 再做 |
| **`policy.cpp:37`、`:59`** | `hard_denied()` 用 `ssh::is_sensitive()` 检查所有工具的全部参数；`exec` 的只读判断对任何 target 都用 SSH 的只读命令表 `ssh::is_read_only()` | **下版改（0.7.4）**：SSH 专用的判断放回 ssh 包，Policy 通过 `Package` 的钩子（例如 `read_only(tool, args)`、`sensitive(args)`）问扩展包。只读命令表同时决定提示词里 C11 的 `confirm_required` 写法，和提示词瘦身一起改 |
| `lua_vm.cpp:573` | 程序里不能调 `lua://` | 合格：防止程序里再起一个代码模式 |
| **`lua_vm.cpp:587`** | 程序里访问 `http://`、`https://` 必须在 `@uses` 里声明，规则写在 lua_vm 里 | **下版改（0.7.6）**：做成扩展包的声明（「这个地址在程序里使用需要声明」），lua_vm 只查声明 |
| `lua_vm.cpp:612` | 对流地址的一次 read 返回字段表 | 合格：按 `registry::stream_spec` 判断，不认具体地址 |
| `lua_vm.cpp:699-702` | `vat.after` 只接受 `prog://`，内部就是 `write trigger://new` | 合格：正是「Lua 函数是资源地址的另一种写法」（P3） |
| **`registry.cpp:221`** | discover 列出流时提示「程序里用 `{"stream": 它, "as": 名字}` 取最新值」 | **本版改**：这是 v0.7 前 JSON 程序的写法，已经不存在；改成「程序里用 `vat.stream(地址)` 取最新值」 |
| `worker.cpp:181`、`:379` | SSH 页固定读写 `tui://current` | 合格，记录即可（见第 1 节） |
| `ui.cpp:869` | 程序页打开本地应用时发一次 `write ui://apps/名字/open` 工具调用 | 合格：界面通过资源地址做事，是该有的样子 |

## 汇总

| # | 发现 | 位置 | 结论 |
|:--|:--|:--|:--|
| 1 | 「失败」靠文字前缀判断，三处各写一份且不一致 | `agent.cpp:440`、`:548-550`，`lua_vm.cpp:603` | **本版改** |
| 2 | discover 对流的提示还是 JSON 程序的旧写法 | `registry.cpp:221` | **本版改** |
| 3 | settings 用法的状态栏可选项缺「后台失败」 | `packages/settings_package.cpp:21` | **本版改** |
| 4 | 两套「布局变了就清屏」、渲染分三条路 | `ui.cpp:67`、`:836-840`、`:953-1040` | 下版改（0.7.5） |
| 5 | 按键处理是一个按页面 if 链的大函数；页面切换、屏幕模型按页面写死 | `ui.cpp:1076-1105`、`:1186-1389`、`:1530-1589` | 下版改（0.7.5） |
| 6 | 四套底栏、两套折行（两套都产生大量小块分配） | `ui.cpp:99`、`:450`、`:740`，`ui_kit.cpp:142`，`settings_page.cpp:156`，`ui_overlay.cpp:365`、`:377` | 下版改（0.7.5） |
| 7 | 五种屏幕内容各自直接调 M5GFX；画布指令表可作为 `gfx` 的原语 | `ui_overlay.cpp:41`、`:61`、`:192-318`、`:330`，`ui.cpp:786` | 下版改（0.7.5） |
| 8 | Graphics 页（程序整屏画面）作为 OS 态浮层存在 | `ui.cpp:43-44`、`:1405-1430` | 下版改（0.8） |
| 9 | local 认领全部 `device://local/*`、`memory://*`，前缀重叠靠注册顺序 | `packages/local_package.cpp`、`packages/builtin.cpp:37-40` | 下版改（0.7.4） |
| 10 | 本机硬件、设置、位置、定时、memory、agent 的地址分散或重名；display / hardware 的别名表 | 第 3 节 | 下版改（0.7.4） |
| 11 | kCorePrompt 的智能家居例子（C12–C17）是场景特例，并与第五条规矩相反 | `agent_prompt.h:12` | 下版改（0.7.4） |
| 12 | ssh、tmux、agents、local 用法里的评测原句和语音纠错 | 对应包的 `kUsage` | 下版改（0.7.4）；语音纠错做成数据 |
| 13 | gps 与 place 都声称负责「我在哪」 | `packages/gps_package.cpp:16`、`packages/place_package.cpp:11` | 下版改（0.7.4） |
| 14 | ui、program 的用法把完整写法放进每次请求 | `packages/ui_package.cpp:167`、`packages/program_package.cpp:25` | 下版改（0.7.4）：挪到 `*://help` 按需读 |
| 15 | Policy 里写死 SSH 的敏感词和只读命令表 | `policy.cpp:37`、`:59` | 下版改（0.7.4） |
| 16 | 程序访问 http(s) 要声明的规则写在 lua_vm 里 | `lua_vm.cpp:587` | 下版改（0.7.6） |
| 17 | 扩展包只构造数据、自己不画；`ui_canvas`、`ui_panel` 只解析 | `packages/display_package.cpp`、`packages/ui_package.cpp`、`ui_canvas.cpp`、`ui_panel.cpp` | 合格，记录即可 |
| 18 | 列表大多共用 `CursorSlide` + `scrolled_rows` | `ui_kit.h`，`ui.cpp:617`、`:763`，`settings_page.cpp:185` | 合格，记录即可 |
| 19 | 注册中心按 `supports()` 分发；`vat.after` 是 `trigger://new` 的写法；界面通过资源地址打开应用 | `registry.cpp`、`lua_vm.cpp:699`、`ui.cpp:869` | 合格，记录即可 |
| 20 | SSH 页固定绑定 `tui://current` | `worker.cpp:181`、`:379` | 合格，记录即可 |
