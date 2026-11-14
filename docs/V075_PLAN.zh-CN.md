# 0.7.5 计划：屏幕绘制统一（gfx）

> 状态：**已确认（2026-11-13）**，按本文开发。
> 来源：`docs/EXTENSIBILITY_AUDIT.zh-CN.md` 标为「0.7.5 改」的 4 条（渲染三条路与两套清屏、按键大函数、四套底栏与两套折行、五种屏幕内容各自调 M5GFX），0.7.4 留下的程序短写法（A12），以及用户要求：**gfx 调用时页面刷新、渲染不闪屏，要有测试保证**。
> 属于哪一态：OS 态（系统页面、面板、画布、程序在图形页上画）。程序整屏独占留给 0.8。

## 一、现状

| 问题 | 位置 | 后果 |
|:--|:--|:--|
| 画布动画每帧先 `fillScreen(kBg)` 再推内存帧 | `ui_overlay.cpp` `draw_overlay()` → `draw_canvas()` | 动画每帧闪一下底色 |
| 面板、大字、二维码、图片、横幅、程序控制台直接画在屏幕上，先清后画 | `ui_overlay.cpp` 五个分支、`ui.cpp` `draw_console()` | 刷新（面板 `source` 定时刷新、控制台输出）时闪，形状一个个出现 |
| 渲染三条路：图形页 `draw_overlay`、程序页 `program_layout`、其余 `paint` + `drawn_layout`；设置页、录音电平条又各自直接画 | `ui.cpp`、`settings_page.cpp`、`ui_recorder.cpp` | 清屏判断两套，v0.7.2 的底栏残留就出在这里 |
| 按键是一个按页面 if 链的大函数；切页、网页接口的屏幕模型按页面写死 | `ui.cpp` `dispatch_keys()`、`switch_page()`、`screen_text()` | 加一个页面要改好几处 |
| 底栏四套、折行两套（逐字符拼 `String`，大量小块分配） | `ui.cpp`、`ui_kit.cpp`、`settings_page.cpp`、`ui_overlay.cpp` | 样式不一、碎片 |
| 程序没有绘图接口：只能把整段 JSON 写进 `ui://canvas/名字` | `packages/ui_package.cpp` | 程序做不了逐帧动画；每次更新都要重新解析整份 JSON |
| 画布借 32 KB 的整屏 8 位帧（`arena` 的 `canvas_frame`） | `ui_overlay.cpp` `make_frame()` | 内存紧时退回直接画（更闪） |

现成可复用的：条带缓冲（`ui_kit::band`，240×16×16 位 = 7.5 KB，先画后推），画布指令表（`ui_canvas`，和 M5GFX 同名同参、白名单、上限，主机可测）。

## 二、不闪屏的定义（验收标准）

一次屏幕更新（下称「一帧」）满足下面三条就算不闪：

1. **不先清后画**：帧内任何一个像素最多被写一次，写入的就是它最终的颜色（屏幕上永远看不到底色闪一下、形状一个个出现）。
2. **内容没变的地方不写**：状态栏时钟走一格，只有时钟所在的那一条被推送。
3. **不同区域互不覆盖**：页面正文不会画到状态栏、底栏、吉祥物角落上，反过来也一样。

## 二点五、补充：详细页实时步骤与思考链（2026-11-14 用户要求，确认放进 0.7.5）

现状：详细页的步骤来自任务结束时写进卡的记录，运行中只显示「（进行中）」；设备请求模型时 `reasoning_effort` 为 `none`，没有思考内容。

- **实时步骤**：`ui_state` 给正在运行的那条记录维护一个实时步骤列表（最多 16 条，每条 240 字节以内）。Agent 在模型返回（思考、附带的文字）、开始执行一个调用、等用户确认、拿到结果（决定和结果开头）时各加一条，并更新版本号。
- **详细页跟着刷新**：详细页打开时，版本一变就重建；停在底部时跟着新内容滚到底，翻上去看时不动。
- **结束后也能看**：任务结束时把这份列表存进记录（`live`），之后打开详细页看到的和运行时一致；没有 `live` 的旧记录照旧显示。
- **思考链是设置项**：`settings://reasoning`（开 / 关，默认关），设置页「显示思考」一页二选一（和「动画」同样的做法）。打开时请求带 `reasoning_effort: "low"`，模型返回的思考内容进实时步骤；关时和以前完全一样。思考内容要带回本轮后续请求（见下方实现记录）。实测简单指令打开后每轮约 1 秒、多 5–20 个 token，思考是一两句英文；上线前用意图用例对照开和关的准确率。

**实现记录（2026-11-14）**

- `agent::Progress` 由一个阶段回调改为结构体：`stage`（时间线状态行，同以前）和 `step(kind, text)`（详细页）。步骤种类：`T` 思考、`S` 随调用附带的文字、`C` 开始一个调用、`W` 等用户确认或输入、`R` 结果、`X` 被拦截或未执行。被确认暂停的调用恢复后不再重复 `C`（`QueuedCall::announced`）。一行调用的写法 `agent::describe_call` 由 worker 的 `steps_json` 挪来，两边共用。
- `ui_state`：实时步骤列表放在 `Model` 之外（快照不复制），`live_version()` 在加步骤、等待、结束时变化。结束时存进记录的 `live` 只保留最新的约 1.2 KB（每条 160 字节）：开机读回时间线只读文件最后 12 KB，记录太大会挤掉旧记录。
- 详细页：版本变了就重建，放在 UI 主循环里而不是绘制里（熄屏时 `render()` 直接返回，网页接口读到的详细页会是旧的）；停在底部就跟到底，翻上去不动；打开一条进行中的记录直接停在最新一步。
- **思考模式的回传规则**（直连 API 实测）：本轮（最后一条用户消息之后）每条带工具调用的 assistant 消息都必须带 `reasoning_content` 字段，否则 HTTP 400「The `reasoning_content` in the thinking mode must be passed back to the API」。字段内容不检查，空串也行；历史轮不带不报错；思考关时带着也不报错。设备上的触发方式是：任务第一轮在设置关时生成，执行中把设置打开，第二轮就 400。现在思考开着或这一轮有思考时，把思考原文（≤1500 字节）放回该消息。
- 意图对照（`prompt_lab --reasoning low`，新增参数，带回规则同固件）：全量 107×2 思考开 212/214、关 210/214（关的 4 次失败是 remember_note、trigger_once_action，各单独 20 次 20/20，是波动）；思考开时 `beep_twice`、`pcm:ambiguous_temperature` 两条各 10 次 16/20（关时 20/20）：模型会先 read 一下再动手。设置默认关。
- 设置包索引行加了 `reasoning`，`remember_note` 新旧索引各 20 次都是 20/20。
- E2E：S4 `ui_detail_steps`（真实任务结束后详细页含调用行）、S11 `settings_reasoning`。

## 三、设计

### G1 合成器：所有绘制只有一个出口

- 屏幕划成固定的**区域**：顶栏、正文、底栏、吉祥物角（32×32）。整屏容器（图形页）占满全屏，吉祥物角此时让出。
- 每个区域有一个**绘制函数**（往一个画面上画，不关心画在哪）。合成器按 16 行一条从上到下：在条带缓冲里先填底色、再执行落在这一条里的所有绘制（自动裁剪）、算这一条的哈希，和上一帧这一条的哈希相同就跳过，不同才整条推送。
- 屏幕上只发生「推送一整条」，不再有 `fillScreen`、`fillRect` 直接打在屏幕上。
- 页面切换、浮层打开关闭，都只是下一帧换了绘制函数，同样逐条合成，所以没有清屏的那一下。
- 画布不再借 32 KB 整屏帧（条带复用现有的 7.5 KB）；`FIRMWARE_RULES` §4「不使用全屏离屏缓冲」继续成立，「动画刷新 ≥ 400 ms」改为按实测帧率定（见 E0）。
- 录音电平条、设置页、启动画面也改为区域绘制函数（启动画面在合成器起来之前，单独列为例外并写明）。

### G2 gfx 扩展包：绘制原语

原语就是现在的画布指令表（再补 `measure` 量文字宽度、屏幕尺寸和配色），两面一一对应（`CLAUDE.md` 第三节）：

| Agent 面（资源地址） | 程序面（Lua） | 说明 |
|:--|:--|:--|
| `write gfx://canvas/名字` content = 画布 JSON（ops、source、animate、vars、update，同现在） | — | 由 `ui://canvas/名字` 改名而来 |
| `write gfx://canvas/名字/close` | — | 关闭 |
| `write gfx://frame` content = ops 数组 | `gfx.clear(颜色)`、`gfx.rect(…)`、`gfx.text(…)` … 记进本帧；`gfx.present()` 一次交给合成器 | 程序逐帧画；`present` 之前屏幕不动，所以不会画到一半被看见 |
| `read gfx://screen` | `gfx.size()`、`gfx.colors()` | 宽高、主题色 |
| `read gfx://measure?text=…&size=…` | `gfx.measure(文字, 大小)` | 文字像素宽度 |

- `gfx.*` 是 `gfx://frame` 的另一种写法，固件里直接绑定（`CLAUDE.md`：调用频繁的函数可以直接绑定，权限和确认规则相同）；屏幕属于「本机可撤销」，程序不用声明 `@uses`。
- 这也就是 0.7.4 留下的「程序短写法」：只给绘图这一类高频调用，不再泛化。
- 一帧的指令数、文字长度沿用画布的上限（64 条、120 字），超了 `present` 返回 `nil, 原因`。

### G3 容器：几种现成的画法

大字、二维码、图片、面板、横幅、程序控制台，改成用 gfx 原语写好的**容器**（各自是一个绘制函数），放在三种位置之一：整屏、正文区、顶栏或底栏。`device://local/display/*` 和 `ui://panel/*`、`ui://apps/*` 对外的地址不变，只是内部换成容器；「任意键返回」这类提示统一交给底栏组件。

### G4 页面是数据

页面 = `{名字, 进入, 正文绘制, 底栏内容, 按键, 屏幕模型}` 一行表。`dispatch_keys()` 只处理全局键（Esc、Fn+H、Fn+W、Fn+A），其余交给当前页面；`switch_page()`、`next_page()`、网页接口的 `screen_text()` 改为查表。新增页面只加一行表、不改分派函数。

### G5 一个底栏组件、一套折行

底栏组件接一组「键 + 说明」，按宽度省略；折行合成一个函数，按字节偏移切、不反复 `substring`，给时间线、终端、控制台、面板共用。

## 四、怎么保证不闪（测试）

| 编号 | 测什么 | 怎么测 | 在哪里跑 |
|:--|:--|:--|:--|
| T1 | 第二节三条规则 | 合成器的条带调度与哈希、区域划分写成不依赖 M5GFX 的代码；主机上用一个记录推送的假屏幕，跑一组场景（页面切换、浮层开关、面板定时刷新、画布动画 60 帧、程序 `present` 连发、状态栏时钟走动、吉祥物动画），断言：从未出现整屏或区域清除；每帧推送的条带互不重叠；内容没变的条带没有推送；正文从不写进状态栏、底栏、吉祥物角 | `make host-test` |
| T2 | 没有绕过合成器的绘制 | `tools/check_ui.py`：`M5Cardputer.Display` / `fillScreen` / `fillRect` 等直接画屏只允许出现在合成器和启动画面里（白名单写在脚本里），其余文件出现即失败 | `make host-test` |
| T3 | 设备上真的这样 | 状态里加 `display`：帧数、推送条数、跳过条数、单帧最长耗时、近 1 秒帧率。E2E 新增一个阶段：跑 gfx 动画程序 3 秒（记录帧率和单帧最长耗时，不设过线）、面板定时刷新 10 秒（每次只推变了的条）、状态栏静止 5 秒（只推时钟那一条）、切四个页面（每次切换推送 ≤ 9 条、没有清屏） | `tools/vat_e2e.py` |
| T4 | 画出来的对 | 上面每个场景结束时 `/api/screenshot` 和预期比对（关键像素、文字区域非空） | `tools/vat_e2e.py` |
| U | 肉眼 | 交付前在设备上看一遍：动画、面板刷新、切页、录音电平条 | 人工，结论写进本文 |

### G2b 完整的 Lua 薄层（2026-11-14 用户确认）

用户要求：gfx 的 Lua 写法完整复刻 M5GFX，与 M5GFX 同名同参。

M5GFX 绘图基类 `LGFXBase` 约 180 个公开函数，分类与处理：

| 类别 | 例子 | 处理 |
|:--|:--|:--|
| A 形状 | fillRect、drawCircle、fillArc、drawEllipseArc、drawWideLine、drawWedgeLine、drawSpot、fillSmoothCircle、fillSmoothRoundRect、fillRectAlpha、drawGradientH/VLine、fillGradientRect、drawCircleHelper、fillAffine… | 全部，同名同参；颜色省略时用 setColor 的当前色 |
| B 文字 | drawString/CentreString/CenterString/RightString、drawNumber、drawFloat、drawChar、print、println、printf、setCursor、setTextColor/Size/Datum/Wrap/Padding/Scroll、setFont、setTextFont | 全部；字体只能用固件里链接的：cn12、cn14（中文）与 M5GFX 的 Font0、Font2、Font4、Font6、Font7、Font8 |
| C 图片与位图 | drawPng/Jpg/Bmp（/vat/ 下文件）、drawBitmap、drawXBitmap、pushImage、pushImageRotateZoom、qrcode | 全部；一帧的图片和像素数据合计不超过 32 KB |
| F 颜色、裁剪、查询 | setColor、setBaseColor、setClipRect、clearClipRect、width、height、textWidth、textLength、fontHeight、fontWidth、getCursorX/Y、color565/888/332、TFT_ 颜色常量 | 全部 |
| D 依赖已有像素 | readPixel、readRect、floodFill、copyRect、scroll、setScrollRect | 0.8 独占态（要程序独占的整屏缓冲） |
| E 离屏画布 | createSprite、pushSprite、pushRotateZoom… | 0.8 独占态 |
| G 硬件与底层 | startWrite/endWrite、writePixels、DMA、setWindow、setRotation、loadFont（读字库文件）、EPD、事务 | 不提供：绕过合成器就破坏「一个出口、不闪屏」 |

做法：

- **一张函数表**（`gfx_api`，主机可测）：每行是函数名、参数类型串（整数、小数、颜色、文字、字体、布尔、对齐方式、位图、像素数据、/vat/ 下图片；可选参数与重载）、示例参数、类别（画、状态、查询）。Lua 绑定、`gfx://frame` 与画布 JSON 的校验、设备上的执行、`read gfx://help` 的函数清单、主机测试全部由这张表驱动，加函数只加一行。
- **状态**：setColor、setTextColor、setFont、setTextSize、setTextDatum、setTextWrap、setTextPadding、setClipRect、光标等在帧里按顺序执行（合成器每条带从同一初始状态重放，各条一致）；`present()` 之后的下一帧开头继承上一帧结束时的状态，与 M5GFX 在屏幕上连续画的效果一致。
- **查询**：textWidth、fontHeight、getCursorX 等要在 Lua 里立即返回。程序的每个状态和文字调用同时在一个「影子画面」上执行（共用条带的写法：虚拟 240×135、裁剪区为空，只走字体和光标逻辑，不写像素），查询读它的真实状态。
- **颜色**：与 M5GFX 一样接受数字（按 RGB565）、`color565(r, g, b)`、`TFT_RED` 这类常量，另外接受 `'#RRGGBB'` 和颜色名；`color888` 返回 `'#RRGGBB'`（Lua 的数字不分 16 位和 32 位，按数字给会和 RGB565 混淆）。
- FeiyuOS 自己的几条保留：`progressBar`、`rotate`（只作用于原来支持旋转的形状）、`drawQr`、画布里的 `drawImage`。
- **测试**：主机测试把表里每个函数用示例参数解析一遍、参数个数和类型不对时报错；设备 E2E 用一个程序把表里每个函数各调一次（参数取自表里的示例），不报错且画面非空；意图用例检查「画图」请求的第一步调用 gfx；设备 E2E 让肥鱼自己写一个 gfx 程序并运行，检查声明 `-- @uses gfx`、调用 `present()`、运行成功、期间屏幕合成了多帧。

### G2b 实现记录（2026-11-14）

- `gfx_api`（主机可测）：76 个函数一行一个，参数类型串含可选与重载（如 `drawBezier` 的 4 点写法颜色必填，同 M5GFX）；`read gfx://help` 由它生成。画布和 `gfx://frame` 的指令里凡是 M5GFX 同名函数都按它校验，变成 `Kind::Call`；FeiyuOS 自己的 `drawString`（带选项对象）、`drawQr`、`drawImage`、`progressBar`、`rotate` 保留（卡上已存的画布应用照常能用）。
- `gfx_exec`（设备）：按表执行，颜色省略时调不带颜色的重载；位图、像素、图片文件在后台任务里先转成 blob（一帧合计不超过 32 KB），执行时按编号取；位图和像素先核对长度。
- 合成器的「虚拟整屏高」条带靠裁剪区挡住越界写入，M5GFX 的抗锯齿线（`drawSmoothLine`、`drawWideLine`、`drawWedgeLine`、`drawSpot`）实测会写到条带内存之外、把堆写坏（下一次 malloc 才崩，崩在哪个核都有）。这几个，以及会读回像素或成块写的（`fillSmoothCircle`、`fillSmoothRoundRect`、`fillRectAlpha`、`pushImage`、`pushImageRotateZoom`、`fillAffine`、`drawPng/Jpg/Bmp`），改为在真实的 16 行条带上画、y 坐标减去条带顶边（`compose::on_real_band`），表里记着每个函数哪些参数是 y。
- Lua：`gfx.<名字>` 第一次用到时才建（`__index`），一个程序只付它用到的几个的内存（一次全建 80 多个闭包时，调用全部函数的程序报内存不够）。状态（字体、颜色、对齐、裁剪、光标、旋转）跨帧继承；画图调用带了可选颜色时 M5GFX 会顺手 setColor，也记为状态。查询在 1 位 4 KB 的「影子画面」上读真实状态（textWidth、fontHeight、getCursorX…），只在程序运行期间存在。
- 默认状态的颜色要写成 `uint16_t`：`0xFFFFu` 是 32 位，M5GFX 当成 RGB888 的青色。
- 索引行措辞会影响无关意图：gfx 的索引写成「画图（函数同 M5GFX，清单 read gfx://help）…」时，「解锁前门门锁。」从 5/5 掉到 0/5（模型先追问或去 discover）；换回原来的短索引、末尾加「函数清单 read gfx://help」后 5/5，画图用例 15/15，全部意图 107 × 2：214/214。
- 设备验证（E2E S13 新增）：`gfx_all_functions` 从 `read gfx://help` 取每个函数的例子，每 30 条一帧全部调用一遍，不崩溃；`gfx_queries` 查询值正确；`gfx_state_across_frames` 两种跨帧颜色；`agent_writes_gfx` 肥鱼自己写 gfx 程序并运行（声明 `@uses gfx`、调用 `present()`、运行期间合成 42 帧）。意图用例新增 3 条（`tests/cases/v075_gfx_cases.json`）。

### G2 实现记录（2026-11-14）

- 地址：`gfx://canvas/名字`（由 `ui://canvas/` 迁来，`address::kVersion` 3，旧地址报错给新写法）、`gfx://frame`、`gfx://screen`、`gfx://measure?text=…&size=…`。面板和画布的「打开期间按 source 刷新」从 ui 包抽成 `ui_live`，两个包共用（本地应用里也可以存画布）。
- 程序面：`-- @uses <包名>` 是通用写法（`lua_meta::Header::libraries`，不需要确认），程序启动时调用该包的 `Package::open_lua()` 注册 Lua 函数，没有这个包或它没有 Lua 写法时直接报错。gfx 注册 `gfx.<指令名>`（与画布指令同名同参，不含 drawImage）、`gfx.present()`、`gfx.size()`、`gfx.measure()`、`gfx.close()`。`present()` 先按 `write gfx://frame` 过一次权限判断，再把这一帧交给界面；上一帧还没被界面取走时最多等 100 ms（帧率自然对齐到屏幕）。
- 每次运行的第一帧带 `raise`：即使上一次运行留下的画面还开着，也会切回图形页；同一次运行里后面的帧不再切页（用户切走就不拉回）。主循环改为先处理「前台程序开始运行，显示控制台」，再取画面。
- 实测：程序不停顿连画 90 帧 1.9 秒，合成 91 帧（全部上屏，约 47 帧/秒）；每帧 `vat.sleep(30)` 时 60 帧 2.1 秒、合成 61 帧（约 29 帧/秒）；每帧平均推 4.2 条（弹球和帧号文字所在的条）。

### G4、G5 实现记录（2026-11-14）

- 页面表 `PageSpec`：名字、网页 id、能否进入、进入、离开、渲染、独占按键（Agent 的密码输入、图形页的画面按键）、Fn 组合键、普通按键、列表视图的绘制、网页屏幕模型的标题和正文。`dispatch_keys()` 只留全局键（设置页、Esc、Fn+空格/H/W/A、Opt+, /.、帮助视图），切页、Btn0 轮换、渲染、`screen_text()` 都查表。顺带修掉：图形页没有画面时按键会落进 SSH 页的打字分支。
- 一套折行 `ui::wrap(g, text, width, max_lines)`：逐个字形量宽（栈上的小缓冲，不为每个字建 `String`），空段落是一行空行，末尾的换行不多出一行，截断时最后一行以「…」结尾；替换原来的 `wrap()` 和 `wrap_text()`。
- 一个底栏 `ui::footer(g, {文字, 颜色, 对齐, 右侧提示}, 宽度)`：程序页、时间线（在吉祥物左边，草稿带「Enter 发送」）、帮助、面板、大字浮层、设置页都用它。

### 设备验证（T3、T4，E2E S13）

S13 十项：静置 5 秒没有整屏重画；切四个页面每次一帧、整帧推送不超过 9 条（`frame_pushed`，吉祥物这类局部更新单独计）；页面往返后截图一致（除顶栏和吉祥物角）；`gfx://frame` 的颜色落在画的位置；程序 60 帧全部上屏（合成 61–62 帧，每帧推 4.1–4.2 条，约 23 帧/秒含 E2E 轮询）；画布动画每帧推 1.4–1.5 条；面板按 source 刷新时 9 条全部跳过（电量没变）；旧地址 `ui://canvas` 报错给新写法；代码模式可用 gfx。

### 内存（发布档 E2E S8 `heap_low_water`，线 40 KB）

- 三轮整轮：35.9 KB（排版跨帧常驻时）、33.8 KB（改为 `compose::PerUpdate`，每次更新后释放）、40.4 KB。同一构建上浮动约 ±4 KB。
- 同条件对照（`--phases all:s5,s6,s7,s8`，同一天）：0.7.4 44.8 KB，0.7.5 40.4 KB，差 4.4 KB；开机静置、走完各页后的空闲堆差约 3.6 KB；静态内存只多 392 字节。
- 合成时的最低堆（`display.min_heap`，每条带画完时取样）整轮最低 48.2 KB，从来不是全局低点；堆追踪构建（`tools/heaptrace.py`）从开机起没释放的 8.7 KB 里没有合成器、gfx、排版缓存的分配（最大的是程序列表缓存，0.7.4 也有）。整轮低点出现在 S8 长输出任务的模型请求一侧，和重启前积累的对话历史有关。
- 结论：这一版过线但余量小；省内存的下一步候选是离开程序页时释放程序列表缓存（约 5.5 KB，页面表已有 leave 钩子），没有做，留待需要时。

### 提示词

实验台 `prompt_lab.py` 原来只认写在同一行的 `index()`，gfx 的索引写成两行时被当成常驻包，拼出的提示词虚胖 1500 多字（设备上没有这个问题）。正则改为认跨行和多段拼接，gfx 的索引也改成单行。修正后系统提示 7061 字（0.7.4 7265），首轮约 4500 token；意图用例 104 × 3：312/312。

## 五、先量值（E0）

动手前在设备上量：一条 16 行的条带执行 N 条指令（N = 10、30、64，含中文文字）要多久，整屏 9 条合成一帧要多久。数据记进本文，留作以后定帧率目标；这一版不以帧率为过线标准（见第七节）。

### E0 实测（2026-11-14，`/api/action compose_bench`：当前页面整帧合成 10 次取平均）

| 页面 | 第一版：全推 / 内容没变 | 按条剔除 + 排版每帧一次 | 再加时间线排版按数据版本缓存 |
|:--|:--|:--|:--|
| Agent（时间线） | 194 / 179 ms | 63 / 49 ms | 54 / 40 ms |
| 帮助 | 98 / 83 ms | 38 / 23 ms | — |
| SSH | 57 / 43 ms | 23 / 10 ms | — |
| 程序 | 89 / 75 ms | 36 / 22 ms | — |

- 推送很便宜：9 条全推只比全部跳过多约 14 ms（每条 7.5 KB，约 1.6 ms）。
- 慢在绘制函数被每条带各跑一遍：第一版里每条都把整页排版重算一遍（`fit`、`wrap` 量字宽），状态栏和不相交的行也照画。办法是两条通用约定：`compose::visible(g, y, h)` 让绘制函数跳过和当前条带不相交的行；排版结果按 `compose::frame_number()`（或数据版本）缓存，一帧里的 9 条带共用。
- 剩下的主要是中文字形的软件渲染（跨两条带的行画两次）。

浮层和设置页接入后（同一方法，排版按帧缓存、按条剔除）：大字 25 ms、面板 28 ms（只动光标时只合成选项那几行，7 ms）、静态画布 19 ms、设置页菜单约 35 ms。二维码第一版 129 ms（每条带都重新编码），改为按内容缓存编码结果、每条只画本条的模块行后 46 ms。画布动画（帧号驱动或 Lua `update`）每帧 3–8 ms，约 29 帧/秒（画布最快 33 ms 一帧），每帧只推变化的条带。画布不再借 32 KB 的整屏 8 位帧。


## 六、地址改动（按扩展包指南第 5 章）

- 登记新协议头 `gfx`（归属 gfx 包，管绘制原语与画布）。
- `ui://canvas/名字` → `gfx://canvas/名字`：写进 5.6 废弃写法表，开机改写卡上数据（`address::rewrite`，`kVersion` 升到 3），调用旧地址报错给新写法。
- `ui://panel`、`ui://apps`、`device://local/display/*` 不变。

## 七、决定（2026-11-13 用户确认）

1. 画布改名：`ui://canvas/名字` → `gfx://canvas/名字`，画图归 gfx，面板和本地应用留在 `ui://`。
2. 程序画图：逐帧「记指令 → `gfx.present()` 一次上屏」，不 `present` 屏幕不动。
3. 帧率：这一版先保证渲染流程（第二节三条规则 + T1–T4），帧率不设过线标准；E0 照量，数据写进本文，留作以后定目标。

## 不在这次范围

- 程序独占整屏、音频、游戏级实时：0.8 独占态。
- 程序之间共用数据：0.7.6。

## 变更记录

| 日期 | 改动 |
|:--|:--|
| 2026-11-13 | 设计稿；用户确认三处决定 |
